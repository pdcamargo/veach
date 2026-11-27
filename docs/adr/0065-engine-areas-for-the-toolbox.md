# 0065: Engine areas for the toolbox: frames, physics, procgen, voxel; what stays in space/

Date: 2026-11-27

## Status

Accepted.

## Context

ADR [0042](0042-space-toolbox-layering.md) puts the toolbox in `space/` and says generic needs move down. The research
found many: large-world positions, strict floating point, a job system, physics, procgen, voxel terrain, runtime meshes,
reflection of arrays and 64-bit values, persistent ids. Each needs a home before code is written. The engine today is
`core ← ecs ← platform ← asset ← script` and `platform ← rhi ← render`, with `runtime` depending on everything
(`engine/CMakeLists.txt`). Research: all twelve docs in [../research/](../research/), and the dev lead's notes.

## Decision

**What goes where in the existing areas:**

| Capability | Area | Notes |
|---|---|---|
| `veach::dmath` (correctly rounded transcendentals) | `engine/core` (`veach/core/dmath.hpp`) | Vendored implementation private to core ([0043](0043-determinism-tiers-and-strict-floating-point.md)) |
| Counter-based hash RNG `veach::hash`, the stagger helper for clock-aligned ticks | `engine/core` (`veach/core/hash.hpp`) | Next to `veach::Random`, which stays for live gameplay ([0044](0044-counter-based-hash-randomness.md)) |
| Job system (VCH-015, now in Now) | `engine/core`, with Flecs workers hooked in `engine/ecs` | Blocks procgen, terrain, physics and simulation LOD |
| Reference-frame tree: `ReferenceFrame`, double `Transform.translation`, frame-relative transforms, composition through the lowest common ancestor | `engine/ecs` (`veach/ecs/frames.hpp`, beside `transform.hpp`) | Frames are entities; propagation runs in TransformPropagation ([0045](0045-large-world-positions-and-reference-frames.md)) |
| Camera-relative extraction in double | `engine/render` | RenderWorld holds no absolute positions |
| Luau `Position` userdata, 64-bit values as tagged userdata | `engine/script` | [0046](0046-64-bit-values-across-luau-json-mcp.md) |
| Reflection arrays, int64, entity references (VCH-099); persistent ids and references (VCH-103); canonical JSON | `engine/ecs` | JSON, Luau, MCP and replay all go through the walker |
| MCP tools registered by modules (VCH-100) | `engine/runtime` | Modules add tools; the server stays in runtime |
| Analog input axes, mouse and gamepad (VCH-101) | `engine/platform` | Into `Input` and actions |
| Runtime (non-cooked) meshes (VCH-102) | `engine/asset` (the mesh data), `engine/render` (upload at Extract) | CPU-produced meshes in the cooked meshlet layout; no GPU handles in components |
| RHI blend state, depth bias, comparison samplers, anisotropic filtering (VCH-098) | `engine/rhi` | Metal, DX12 and null |
| Quality presets ([0064](0064-quality-presets.md)) | `engine/runtime` (selection, schema); each area reads its section | |

**Three new engine areas**, each its own static library:

- **`engine/physics`** (`veach::physics`): Jolt private to it ([0058](0058-physics-jolt.md)), scenes per frame,
  tick-scheduled transfers and collider installs, the gravity-force hook, contact events, debug draw, MCP tools.
  Depends on `core` and `ecs`.
- **`engine/procgen`** (`veach::procgen`): the stage runtime and cache with stage `id`, `version` and stability class
  ([0051](0051-staged-generation-pipeline.md), [0055](0055-universe-identity-and-versioning.md)), the field-graph IR,
  its JSON loader and Luau builder, the CPU interpreter, and the Luau procgen sandbox
  ([0052](0052-procgen-runtime.md)). Depends on `core`, `ecs`, `asset` and `script`.
- **`engine/voxel`** (`veach::voxel`): density chunks in octrees, the CPU mesher, LOD selection and streaming, the edit
  store and brush application, the region-file container, and the bridge that hands LOD0 meshes to physics
  ([0053](0053-terrain-density-field.md), [0054](0054-terrain-edits-and-water.md)). Flat-world capable. Depends on
  `core`, `ecs`, `asset`, `procgen` and `physics`.

**The layering rule for them:** `physics`, `procgen` and `voxel` never link `rhi` or `render`. Authoritative work runs
headless on the null RHI. The GPU evaluator for field graphs and GPU far-field meshing, when they come, live in
`engine/render` as render features that consume procgen's IR and voxel's chunk records; they are visual tier and
nothing reads them back into simulation. The dependency order becomes:

```text
core ← ecs ← platform ← asset ← script ← procgen ← voxel
       ecs ← physics ← voxel
             platform ← rhi ← render (may read procgen and voxel data)
runtime depends on everything; veach::veach is the umbrella
```

Networking (transport, codec, `NetId`, interest) becomes `engine/net` when VCH-095 starts
([0056](0056-server-authoritative-multiplayer.md)); nothing is created for it now.

**What stays in `space/`:** the galaxy, universe addresses and their seed paths
([0050](0050-universe-addresses-and-seeds.md)), body kinds, the orbit and time-model specifics on top of the engine tick
([0047](0047-universe-clock.md), [0048](0048-bodies-on-rails.md)), the scale model ([0049](0049-scale-model.md)), the
toolbox's stages and nodes, planetary terrain bands, sea level and the binding of a body frame to a voxel volume,
atmosphere records from composition, gravity sources, flight models and the travel governor
([0059](0059-gravity-flight-travel.md)), simulation domains and tiers ([0057](0057-simulation-lod.md)), and the universe
manifest semantics: genesis, epochs, pins ([0055](0055-universe-identity-and-versioning.md)).

**Checks.** `tools/scripts/check_repo.py` already fails if `engine/` names `space/`. With the first of these areas it
gains two more checks: `physics`, `procgen` and `voxel` must not link `rhi` or `render`, and no file outside
`engine/physics` may include a Jolt header.

## Consequences

- A non-space game can use frames, physics, procgen and voxel terrain without the toolbox.
- Servers and CI get every authoritative system without a GPU, by construction of the build graph.
- More libraries and link-order care.
- Each prerequisite above is a backlog item that lands with its first user, not ahead of it.
- **Validation:** the spikes that first exercise each area: VCH-077 and VCH-078 (`core`: flags, dmath), VCH-079 (frames
  and camera-relative extraction), VCH-082 and VCH-083 (`procgen`), VCH-084 and VCH-088 (`voxel`), VCH-087 (`physics`),
  and VCH-091 (SP-15), the first slice built only from these areas plus `space/`.
- **Reopen if** a spike needs `space/` types inside an engine area (the area's boundary is wrong), or voxel terrain
  needs GPU-authoritative output (then the "never link render" rule and
  [0043](0043-determinism-tiers-and-strict-floating-point.md) are both in question).

## Alternatives considered

- **Everything planetary in `space/`, including physics and voxel terrain.** Violates "generic goes down" and leaves a
  flat voxel game nothing to use.
- **One `engine/world` area for frames, physics, procgen and voxel.** Fewer libraries, but physics would drag Jolt into
  procgen's users and the GPU rule could not be checked per area.
- **Frames as their own area.** The frame tree is the transform system with doubles; splitting it from `ecs` would put
  transform propagation in two places.
- **Voxel without a physics dependency, bridged in `space/`.** A flat voxel game would have to rewrite the collider
  bridge.
