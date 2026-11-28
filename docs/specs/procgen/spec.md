# Procgen: stage runtime, field graphs and the generator sandbox

**Status:** Implemented
**Area:** procgen
**Owner:** dev lead

## Summary

`engine/procgen` (`veach::procgen`) is the engine's generation area. It runs **stages** (pure C++ functions that turn
earlier records into a new record, memoized by address), evaluates **field graphs** (JSON node graphs compiled to a
flat op list and run by a canonical CPU interpreter, in batches, on lattice points), and hosts the **Luau generator
sandbox** for hooks. It is generic: `space/` registers its stages and graphs on it, and a flat voxel game can use it
without the toolbox. ([ADR 0052](../../adr/0052-procgen-runtime.md), [ADR 0065](../../adr/0065-engine-areas-for-the-toolbox.md))

## Motivation

Three spikes proved the parts separately: SP-05's stages S1 and B1 in `space/gen` with their own seed and hash helpers,
SP-06's field-graph compiler and CPU batch interpreter in `tests/sp06`, and SP-07's generator sandbox parked in
`engine/script`. The first slice (VCH-091) needs them as one engine area: terrain streaming (VCH-114) samples
density from a field graph on worker jobs, and the space realization (VCH-115) reads stage records through a cache.
Backlog: VCH-112.

## Requirements

**Area**

- MUST be the static library `veach::procgen`, depending on `veach::core` only (plus Luau and nlohmann/json
  privately), and MUST NOT link or include `rhi` or `render` (`check_repo.py` fails otherwise).
- MUST be canonical code ([ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md)): the same bits on
  macOS arm64 and Windows x64, Debug and Release. Every canonical result has a committed hash in a unit test.

**Stages**

- MUST register a stage with a `namespace:name` id, an integer version, a stability class (`frozen`, `pinned`,
  `refreshable`), a tier (`canonical`, `replay`, `visual`), its typed inputs and a canonical hash of its output.
- MUST keep several versions of one id side by side, and refuse a duplicate id and version (`procgen.stage_invalid`).
- MUST let a stage read only the inputs it declared, each an earlier-registered stage at its own address or at an
  address mapped from it (an ancestor), so a read from a later stage or a cycle cannot be written.
- MUST refuse a stage whose input is in a lower tier (a canonical stage reading a visual product).
- MUST give a stage its seed as `hash(universe_seed, address, hash_id(stage_id), version)`
  ([ADR 0050](../../adr/0050-universe-addresses-and-seeds.md)), and no scale factor.
- MUST cache each output under (stage, address, input hash, manifest fingerprint), where the input hash combines the
  declared inputs' output hashes, so an authored override of an input re-runs everything downstream of it.
- MUST fail a stage as `procgen.stage_failed` naming the stage, its version and the address, and keep nothing.
- MUST be safe to call from many jobs at once, with results that never depend on the number of threads.
- SHOULD bound the cache by entry count, evicting the least recently used entry; overrides are never evicted.

**Field graphs**

- MUST read the JSON IR of [ADR 0052](../../adr/0052-procgen-runtime.md) (`"veach_graph": 1`, `id`, `inputs`, `nodes`,
  `outputs`), checking node types and versions, ports, unit classes on lengths and cycles, with errors that name the
  node and the fix (`procgen.graph_invalid`, `data.unknown_field`, `data.parse_failed`).
- MUST compile a graph for one body (seed, radius, planet meters per realized meter) into an op list whose layout the
  GPU interpreter of SP-06 can read unchanged, with node seeds `hash(body_seed, hash_id(node_id))` and octave seeds
  folded to 32 bits ([ADR 0071](../../adr/0071-field-graph-precision-rules.md) rule 2).
- MUST support one or more named outputs per program, written planar (output-major).
- MUST evaluate on two domains: **lattice points** (integer LOD0 lattice coordinates of a body-fixed lattice) and
  **cube-face regions** on the sphere (SP-06's domain, for coarse maps and previews).
- MUST give a lattice sample as a pure function of its lattice point ([ADR 0074](../../adr/0074-amendments-from-sp-08.md)
  rule 1): the API takes positions and never a level, a node never sees the step or the chunk that asked, and the
  same point gives the same bits from a point list, from a box of any step and origin, and in parallel.
- MUST evaluate in batches bit-equal to evaluating one sample at a time (ADR 0071 rule 3).
- MUST compute the precision-critical products of gradient noise as fused operations (ADR 0071 rule 1): `std::fma`
  with hardware FMA, the exact double route without.
- MUST offer a parallel evaluation of a lattice box on `veach::JobSystem` in fixed chunks.
- SHOULD reach 10 M samples/s per core on the SP-06 terrain graph (ADR 0052's bar).
- SHOULD build the same IR from Luau through the generator sandbox (`procgen.graph`).

**Generator sandbox**

- MUST be `veach::procgen::Sandbox` with SP-07's contract and [ADR 0070](../../adr/0070-generator-sandbox-rules.md)'s
  rules, moved unchanged from `engine/script`, which keeps only the gameplay VM.

## API sketch

```cpp
#include <veach/procgen/field_graph.hpp>
#include <veach/procgen/stage.hpp>
#include <veach/procgen/sandbox.hpp>
namespace pg = veach::procgen;

// ---- Field graphs: parse once (an asset, keyed by hash()), compile per body, evaluate on positions ----
veach::Result<pg::FieldGraph> graph = pg::parse_field_graph(json_text, "terrain.field.json");
veach::Result<pg::FieldProgram> program = pg::compile(*graph, {.seed = body_seed, .radius_planet_m = 6.0e6,
                                                            .planet_m_per_m = 1.0 / s_p});

// LOD0 lattice: point (i, j, k) sits at origin_m + (i, j, k) * spacing_m, body-fixed, realized meters (p_local).
const pg::Lattice lattice{.spacing_m = 0.5, .origin_m = {0.0, 0.0, 0.0}};
// One chunk's corners: first + step * (x, y, z), x fastest. The step picks the points; nodes never see it.
const pg::LatticeBox box{.first = {i0, j0, k0}, .step = 1 << level, .count = {35, 35, 35}};
std::vector<float> density(program->outputs.size() * box.size());         // planar: [output][sample]
pg::evaluate(*program, lattice, box, density);                             // this thread
pg::evaluate(jobs, *program, lattice, box, density);                       // veach::JobSystem, fixed chunks
pg::evaluate(*program, lattice, std::span<const pg::LatticePoint>(points), density);  // any point list
pg::evaluate(*program, face_region, heights);                              // cube-face domain: a square of a face
pg::evaluate(*program, std::span<const std::array<float, 3>>(directions), heights);  // or any unit directions

// ---- Stages: typed, pure, memoized ----
pg::StageRegistry registry;
auto system = registry.add<SystemRecord>({.id = "space:system", .version = 1, .stability = pg::Stability::frozen},
    &system_hash,
    [](const pg::StageContext& ctx, const UniverseRecord& u) { return generate(ctx.seed(), ctx.address, u); },
    pg::from(*universe, pg::root_address));                                // an ancestor at address 0
pg::StageRuntime runtime(registry, {.universe_seed = 42, .manifest_fingerprint = fp, .cache_capacity = 4096});
veach::Result<std::shared_ptr<const SystemRecord>> r = runtime.get(*system, address);
runtime.override(*system, address, authored_record);                       // downstream stages re-run

// ---- Generator hooks ----
pg::Sandbox sandbox;                                                       // one per worker thread
auto hook = sandbox.load(source, "climate.luau");
auto ran = sandbox.run(*hook, {.seed = seed, .params = params, .inputs = inputs, .outputs = outputs});
auto built = sandbox.build_graph(graph_source, "terrain.graph.luau");      // the Luau builder → FieldGraph
```

## Acceptance criteria

- **procgen.AC1** — `veach::procgen` links core only; a `veach/rhi` or `veach/render` include or link in
  `engine/procgen` or `engine/voxel` fails `check_repo.py`. The sandbox lives in `engine/procgen`, and SP-07's tests
  pass unchanged, the climate hook hash `0x7e5315bcd6461c30` (`[sp07]` tests) included.
- **procgen.AC2** — Graph errors name the node and the fix: a bare length's hint names `planet_m`, an unknown version
  names the version this build has, an unknown parameter is `data.unknown_field`, a cycle and a position wired into
  a scalar port are `procgen.graph_invalid`.
- **procgen.AC3** — The SP-06 terrain graph compiles to 8 ops, 11 noise octaves and at most 4 registers, with the
  frequencies and octave seeds SP-06 committed (`0x0D1C6AB3`... for `continents`); another body seed changes only
  the seeds.
- **procgen.AC4** — On SP-06's 128² cube-face region the interpreter's output hashes to `0xFB7136EA481F1F8D`, the
  same in batches of 256 and of 1, and the double route gives `std::fma`'s bits on 600,000 operand triples.
- **procgen.AC5** — On the lattice, a point gives the same bits from a point list, from boxes of step 1, 2 and 4 with
  different first corners, in batches of 256 and of 1, and in parallel on 1 and 4 threads; the output of a fixed
  box hashes to a committed value. `p_planet` and `p_local` lengths are checked against their input's unit class.
- **procgen.AC6** — A program with two outputs writes them planar, each equal to the single-output program's.
- **procgen.AC7** — The registry keeps versions 1 and 2 of one id side by side, refuses a duplicate id and version,
  a malformed id and a canonical stage reading a visual one, all as `procgen.stage_invalid`.
- **procgen.AC8** — The runtime runs a stage once per (stage, address, input hash): a second `get` is a cache hit;
  an override re-runs the stages downstream of it at that address only; a failing stage is `procgen.stage_failed`
  and is not cached; an LRU capacity evicts; `StageContext::seed()` is the ADR 0050 formula.
- **procgen.AC9** — SP-05's S1 and B1, registered by `space/gen` and run through the runtime on 2,000 addresses,
  hash to `0xc28a0456eedc0301` and `0xf2f6a4c18dbe873d` (`[sp05]` tests), the same through the runtime on 1 and 4
  job threads as called directly.
- **procgen.AC10** — The Luau builder, run in the generator sandbox, builds the SP-06 terrain graph with the same
  op list as the JSON file; a builder error is `procgen.graph_invalid` with the script's line.
- **procgen.AC11** — `veach-bench-procgen` reports the interpreter's samples per second per core on the face region
  and on a lattice box (measured, not a test: ADR 0052's bar is 10 M samples/s per core). Measured on one M5 Max
  thread, release preset: 19.2 M samples/s on SP-06's 1024² face, 18.0 M on 35³ lattice chunks of the same
  11-octave graph, 28.1 M for a 7-octave cave graph.
- **procgen.AC12** — `lattice_bounds(program)` gives each output a value range, a Lipschitz constant per realized
  meter and a float-error bound, proven from the op list: on 8,000 points of the cave and hill graphs every
  evaluated value lies in the range and no difference between points 0.5 to 32 m apart exceeds the Lipschitz bound
  (plus twice the error). Terrain uses it to skip chunks that cannot hold a surface (VCH-114).

## Out of scope

- The GPU evaluator: a render feature in `engine/render` that uploads `FieldProgram::words` (SP-06's
  `tests/sp06/shaders` are its prototype).
- Graphs as assets with a loader, hot reload by graph hash, and the MCP tools `procgen_inspect`, `procgen_eval`,
  `procgen_preview` (VCH-120).
- More node types (density primitives such as a sphere distance, domain warp with vec3 registers,
  `veach:sample_cubemap@1`), graph-level `params`, constant deduplication, a public node registry for C++ modules,
  octave fading below `min_planet_wavelength_m`, and explicit SIMD kernels (VCH-121).
- Luau stages and stage overrides by mods (`procgen.stage(...)`, ADR 0052), and the generator type environment for
  `luau-analyze` (ADR 0070 consequences) (VCH-121 lists the Luau half).
- Genesis, epochs, pins and which stage version a body uses: `space/` semantics on top of the registry
  ([ADR 0055](../../adr/0055-universe-identity-and-versioning.md), VCH-094).

## Open questions

None.

## Links

- ADRs: [0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md),
  [0044](../../adr/0044-counter-based-hash-randomness.md), [0050](../../adr/0050-universe-addresses-and-seeds.md),
  [0051](../../adr/0051-staged-generation-pipeline.md), [0052](../../adr/0052-procgen-runtime.md),
  [0055](../../adr/0055-universe-identity-and-versioning.md), [0065](../../adr/0065-engine-areas-for-the-toolbox.md),
  [0070](../../adr/0070-generator-sandbox-rules.md), [0071](../../adr/0071-field-graph-precision-rules.md),
  [0074](../../adr/0074-amendments-from-sp-08.md)
- Spikes: [SP-05](../../research/spikes/sp-05-system-statistics.md), [SP-06](../../research/spikes/sp-06-field-graph.md),
  [SP-07](../../research/spikes/sp-07-luau-generator.md)
- Backlog: VCH-112 (this), VCH-114 (voxel samples density through it), VCH-120, VCH-121
- Guides: [procgen](../../guides/procgen.md)
