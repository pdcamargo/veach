# Voxel terrain: streaming, edits and collider meshes

**Status:** Approved
**Area:** voxel, runtime
**Owner:** dev lead

## Summary

`engine/voxel` (`veach::voxel`) streams a terrain body: one signed density field on a body-local integer lattice,
sampled from a procgen field program, meshed in 32³-cell chunks of a restricted octree around the viewers, under a
per-frame job budget that draws coarser terrain instead of stalling and never draws a hole. It applies brush edits
into override records, and hands LOD0 meshes with their adjacency rings to physics. A thin binding in
`engine/runtime` connects a body to its reference frame, its viewers and the renderer's `MeshStore`. It works for a
planet, an asteroid or a flat world. ([ADR 0053](../../adr/0053-terrain-density-field.md),
[0054](../../adr/0054-terrain-edits-and-water.md), [0065](../../adr/0065-engine-areas-for-the-toolbox.md))

## Motivation

The first slice (VCH-091) flies from orbit to the ground and digs a hole. Spikes proved the parts in throwaway code:
SP-08's mesher and SP-12's brush applier in a small `engine/voxel`, SP-09's octree, streaming, budget and weld check
in the lab, SP-12's dig and collider cooking in `tests/sp12`. VCH-114 makes them one engine area; the space
realization (VCH-115) creates a body per planet, and `engine/physics` (VCH-113) installs its colliders. Backlog:
VCH-114.

## Requirements

**Area**

- MUST be `veach::voxel`, depending on `veach::core` and `veach::procgen`, and MUST NOT link or include `rhi`,
  `render` or Jolt (`check_repo.py`). The renderer and physics reach it through data: chunk records and collider
  meshes.
- MUST be canonical where it is authoritative ([ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md)):
  samples, edits, meshes, their hashes and collider meshes are the same bits on every platform and for any number of
  workers. Selection and scheduling may depend on floating-point viewer positions and the budget; they never change
  what a chunk holds.

**Density**

- MUST sample a chunk from a `DensitySource` whose samples are pure functions of the LOD0 lattice point
  ([ADR 0074](../../adr/0074-amendments-from-sp-08.md) rule 1), as companded int16 codes
  ([ADR 0079](../../adr/0079-amendments-from-sp-09.md) rule 1) plus a u8 material.
- MUST provide `ProgramDensity`: a base shape (sphere, plane or none) plus a scaled output of a procgen
  `FieldProgram`, evaluated one chunk per call ([ADR 0084](../../adr/0084-amendments-from-building-procgen.md)
  rule 4), with an optional material output.
- MUST classify boxes conservatively from `procgen::lattice_bounds` (a center evaluation, the Lipschitz bound, split
  up to `classify_depth` times): a box is `empty` or `solid` only if every sample in it is.

**Selection and streaming**

- MUST select a restricted octree (touching leaves differ by at most one level) refined by distance with ratio λ to
  the nearest of any number of viewers, prune boxes proven empty or solid, and keep a node split until every viewer
  is beyond λ·(1 + hysteresis) child edges.
- MUST give each leaf the faces whose neighbor is finer, and mesh Transvoxel transition cells on exactly those; a
  chunk whose mask changes is remeshed (ADR 0074 rule 2).
- MUST run chunk jobs (sample, overlay edits, mesh, hash, optional payload) on `veach::JobSystem`, at most
  `TerrainStreamingBudget::jobs_per_frame` per update, coarse levels first, across frames by default.
- MUST draw only complete selections: the drawn set changes only when every chunk of the new selection is meshed.
- MUST lower the effective λ while the drawn set lags for more than the budget's patience, down to `min_lambda`, and
  raise it back to the configured λ once the stream keeps up.

**Edits**

- MUST apply ADR 0054's `add@1` and `subtract@1` sphere brushes in integer arithmetic to every LOD0 chunk's owned
  samples within the brush radius plus 2 cells ([ADR 0077](../../adr/0077-amendments-from-sp-12.md) rules 1 to 3),
  sampling a chunk the first time an op touches it, in sequence order, refusing bad ops with
  `voxel.brush_invalid`.
- MUST keep one override record per edited LOD0 chunk (owned samples, a mask, a base hash) and a journal of ops,
  and overlay the records onto chunks of every level at their own lattice points.
- MUST remesh every held chunk and collider an edit may have changed, keeping the old mesh drawn until the new one
  lands.
- MUST restore a saved record over regenerated samples, logging `voxel.base_mismatch` when the base hash differs.

**Colliders**

- MUST select the LOD0 chunks within `collider_reach_m` of each probe's path over `collider_lookahead_s`, mesh them
  and their neighbors on jobs, and submit to a `ColliderSink` the chunks whose mesh or ring changed, each with its
  own triangles then its neighbors' border triangles as adjacency ([ADR 0083](../../adr/0083-terrain-colliders-with-an-adjacency-ring.md)),
  for install at `tick + collider_delay_ticks` (2), in key order, and the chunks no longer needed.

**Binding (engine/runtime)**

- MUST attach a `TerrainBody` to a reference-frame entity ([ADR 0085](../../adr/0085-amendments-from-vch-111.md)):
  viewers are entities with `TerrainViewer`, converted into the body's frame in double each frame.
- MUST upload chunk meshes to the renderer's `MeshStore` (ADR 0072), building them on the chunk's job, adding them on
  the main thread in key order, releasing evicted ones, and extracting the drawn set into render space through
  `RenderSpaceMap`, chunk origins converted in double relative to the render origin.

## API sketch

```cpp
#include <veach/voxel/terrain.hpp>
namespace vx = veach::voxel;

auto density = vx::ProgramDensity::make(program, {.shape = vx::BaseShape::sphere, .radius_m = 637'100.0,
                                                  .density_output = "density", .material_output = "material"});
vx::TerrainSettings settings{.octree = vx::OctreeBounds::around_origin((*density)->max_extent()), .lambda = 4.0};
settings.budget = vx::TerrainStreamingBudget::from_rate(measured_chunks_per_second);
vx::TerrainBody body(*density, settings);

body.set_viewers(viewers);                     // body-local meters
vx::StreamStats stats = body.update(jobs);     // once per frame
for (const vx::ChunkRef& c : body.ready()) {}  // upload; body.evicted(): release; body.drawn(): draw
auto edit = body.apply(op, jobs);              // a BrushOp: Result<EditResult>
body.set_collider_sink(&physics_terrain);      // engine/physics implements vx::ColliderSink
body.update_colliders(probes, tick, jobs);     // ColliderUpdate{install_tick = tick + 2, install, remove}
```

```cpp
#include <veach/runtime/terrain.hpp>
engine.import<veach::TerrainModule>();
auto& terrain = veach::services(world).get<veach::TerrainBodies>();
terrain.add(planet_frame, std::make_shared<vx::TerrainBody>(density, settings), {.materials = {grass, rock}});
camera.add<veach::TerrainViewer>();
```

## Acceptance criteria

- **voxel.AC1** — `engine/voxel` links core and procgen only; an `rhi`, `render` or Jolt include fails
  `check_repo.py`.
- **voxel.AC2** — A level-0 chunk, its level-1 parent and the parent's full-resolution face hold the same samples at
  every shared lattice point; their sample hashes are committed. On a flat world and a sphere, every box classified
  empty or solid has no sign change, and the bounds decide most boxes away from the surface.
  `voxel.density_invalid` names a missing output and lists the program's outputs.
- **voxel.AC3** — The selection around two viewers is restricted, sorted, has level 0 under each viewer, holds only
  boxes that may hold a surface, and marks finer faces only toward split neighbors. Hovering between two positions
  that flip the selection without hysteresis changes it at most once with hysteresis 0.1.
- **voxel.AC4** — A descent from 800 m to 10 m over a 200 m asteroid on 6 jobs per frame draws no hole (0 open
  edges in every drawn set), never empties the drawn set, lowers λ below its configured value without going under
  `min_lambda`, and recovers λ once hovering.
- **voxel.AC5** — The same flight on 1 and 4 workers gives the same drawn hash after every update; the settled
  drawn hash is committed and equals the asynchronous run's.
- **voxel.AC6** — When the viewer moves, chunks whose neighbor changed level are remeshed with transition cells and
  the drawn set has 0 open edges; plain marching cubes across the same levels has open edges.
- **voxel.AC7** — SP-12's six-op dig, applied through the edit store to a source wrapping SP-12's field, reproduces
  SP-12's committed base and edited sample and mesh hashes on 1 and 4 workers; the store's hash is committed; the
  level-1 parent reads the overrides at its points; out-of-order and oversized ops are `voxel.brush_invalid`;
  restored records give the same store hash and a wrong base hash logs `voxel.base_mismatch`.
- **voxel.AC8** — A probe moving at 40 m/s gets LOD0 colliders with adjacency rings (own triangles equal the chunk's
  LOD0 mesh, welded positions) for install at t + 2 in key order, with a committed hash; an unchanged call submits
  nothing; a dig on a chunk border recooks the changed chunks and their ring-fed neighbors; moving away removes them.
- **voxel.AC9** — The binding streams a body in a reference frame around a `TerrainViewer` camera on the null RHI:
  chunk meshes reach the `MeshStore`, the drawn set is extracted as runtime-mesh draws, evicted meshes are removed.
- **voxel.AC10** — The lab's terrain view flies from orbit to 10 m over a planet on Metal with 0 open edges at every
  checkpoint; its screenshots are in `docs/research/spikes/voxel-terrain/`.
- **voxel.AC11** — `veach-bench-terrain` reports chunks per second on 1, 4 and all threads for SP-09's 100 m
  checkpoint with a 19-octave procgen planet, against SP-09's 129 / 457 / 1,678 (measured, not a test).

## Out of scope

- The region-file container and save format for override records (VCH-094); records restore from memory here.
- Brush kinds beyond add and subtract (flatten, smooth, raise, lower, paint) and orientation, strength, falloff.
- A per-subtree "never a hole" policy for sustained overload (SP-09 suggests it); cached samples for remeshes
  (ADR 0079 rule 4's second lever); GPU far-field producers.
- Cooking and installing colliders: `engine/physics` (VCH-113) implements `ColliderSink`.
- Water levels (ADR 0054), debug draw of chunk bounds, MCP tools for terrain.

## Open questions

None.

## Links

- ADRs: [0053](../../adr/0053-terrain-density-field.md), [0054](../../adr/0054-terrain-edits-and-water.md),
  [0065](../../adr/0065-engine-areas-for-the-toolbox.md), [0072](../../adr/0072-runtime-meshes.md),
  [0074](../../adr/0074-amendments-from-sp-08.md), [0077](../../adr/0077-amendments-from-sp-12.md),
  [0079](../../adr/0079-amendments-from-sp-09.md), [0083](../../adr/0083-terrain-colliders-with-an-adjacency-ring.md),
  [0084](../../adr/0084-amendments-from-building-procgen.md), [0085](../../adr/0085-amendments-from-vch-111.md)
- Backlog: VCH-114 (VCH-091 → VCH-111 to VCH-115)
- Guides: [voxel terrain](../../guides/voxel-terrain.md), [procgen](../../guides/procgen.md)
- Findings: [SP-08](../../research/spikes/sp-08-mesher.md), [SP-09](../../research/spikes/sp-09-descent.md),
  [SP-12](../../research/spikes/sp-12-dig-collide.md), [seams sweep](../../research/spikes/seams-sweep.md)
