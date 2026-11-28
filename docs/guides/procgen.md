# Procgen: field graphs, generator hooks and stages

`engine/procgen` (`veach::procgen`) generates content that every machine must agree on: terrain density, stage
records, anything gameplay reads. It has three parts, each for one kind of work
([ADR 0052](../adr/0052-procgen-runtime.md), [spec](../specs/procgen/spec.md)):

| Part | For | Written in | Runs |
|---|---|---|---|
| **Field graphs** | per-sample functions: terrain density, masks, heights (10⁶ to 10⁹ samples) | JSON, or Luau through the builder | the CPU interpreter, in batches, on worker jobs |
| **Generator hooks** | small per-body programs over coarse maps (climate, biomes) | strict Luau in the generator sandbox | one call per body or per map |
| **Stages** | records: a system, a body's physics (1 to 1,000 per body, branchy) | C++ | on demand, memoized by address |

Everything here is **canonical** ([ADR 0043](../adr/0043-determinism-tiers-and-strict-floating-point.md)): the same
bits on macOS arm64 and Windows x64, Debug and Release. Every canonical output has a committed hash in a unit test;
a changed hash is a version bump, never a test update (see [Changing output](#changing-output)).

## Field graphs

### The format

```json
{
  "veach_graph": 1,
  "id": "mymod:caves",
  "inputs": { "p": "p_local" },
  "nodes": [
    { "id": "tunnels", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "m": 24 }, "octaves": 4 } },
    { "id": "rock", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "m": 3 }, "octaves": 3, "gain": 0.4 } },
    { "id": "walls", "type": "veach:ridge@1", "inputs": { "x": "tunnels" }, "params": { "offset": 0.6 } },
    { "id": "density", "type": "veach:add@1", "inputs": { "a": "walls", "b": "rock" } }
  ],
  "outputs": { "density": "density" }
}
```

- **Node ids are stable names.** A node's seed is `hash(body_seed, hash_id(node_id))`, so renaming a node reseeds it
  and adding one leaves the others alone.
- **Node types are versioned** (`veach:fbm@1`). A node whose output changes becomes a new version; the old one stays
  for as long as any universe uses it.
- **Every length has a unit class.** `{"planet_m": 3000000}` scales with the planet and samples `p_planet`;
  `{"m": 3}` never scales and samples `p_local`. A bare number is an error.
- **Inputs** are positions: `p_planet` (the unscaled body-fixed position, so continents sit at the same latitude at
  every scale) or `p_local` (realized meters, so a rock is the same size at every scale)
  ([research 05 §5](../research/05-procgen-graph.md#5-scale-how-features-say-what-they-scale-with)).
- **Outputs** map names to nodes. A graph may have several; a program writes them in name order.

### Nodes

| Type | Ports | Parameters | Value |
|---|---|---|---|
| `veach:fbm@1` | `p` (an input) | `wavelength` (length), `octaves` 1 to 24, `lacunarity` 2, `gain` 0.5, `amplitude` 1 | Σ amplitude·gainᵒ · noise(p · lacunarityᵒ / wavelength), gradient noise roughly in [−1, 1] per octave |
| `veach:ridge@1` | `x` | `offset` 1 | (offset − \|x\|)² |
| `veach:remap@1` | `x` | `from` [lo, hi], `to` [lo, hi] | the linear map of `from` onto `to` (not clamped) |
| `veach:clamp@1` | `x` | `min` 0, `max` 1 | x clamped |
| `veach:blend@1` | `a`, `b`, `t` | | a + t (b − a) |
| `veach:add@1` | `a`, `b` | | a + b |
| `veach:mul@1` | `a`, `b` | | a · b |

More nodes (density primitives, domain warp, coarse-map sampling) and a registry for C++ modules are VCH-121.

### Compiling and evaluating

```cpp
#include <veach/procgen/field_graph.hpp>
namespace pg = veach::procgen;

auto graph = pg::parse_field_graph(json_text, "caves.field.json");          // once: validated IR, graph->hash()
auto program = pg::compile(*graph, {.seed = body_seed,                      // per body: seeds and constants folded
                                    .radius_planet_m = 6.0e6,               //   (cube-face domain only)
                                    .planet_m_per_m = 1.0 / s_p});          //   p_planet = p_local * this

// The LOD0 lattice: point (i, j, k) is at origin_m + (i, j, k) * spacing_m, body-fixed, realized meters.
const pg::Lattice lattice{.spacing_m = 0.5};
// A level-l chunk's corners plus apron: every 2^l-th lattice point, x fastest.
const pg::LatticeBox box{.first = {x0 - step, y0 - step, z0 - step}, .step = step, .count = {35, 35, 35}};
std::vector<float> density(program->outputs.size() * box.size());           // planar: [output][sample]
pg::evaluate(*program, lattice, box, density);                              // on this thread
pg::evaluate(*program, lattice, points, density);                           // or any list of lattice points
pg::evaluate(jobs, *program, lattice, box, density);                        // one big box over veach::JobSystem
```

**Positions, never levels.** A sample is a pure function of its lattice point
([ADR 0074](../adr/0074-amendments-from-sp-08.md) rule 1): no node sees the step, the chunk or the batch that asked,
so two chunks of different levels read the same bits at the points they share, and Transvoxel's seams close. Each
noise octave computes its lattice cell and fraction from the integer point in double, so the result does not depend
on where the chunk starts. Never band-limit a graph by the caller's level; band limits depend on position only.

**Two domains.** Lattices are the domain of terrain density and anything with collision. `pg::FaceRegion` (a square
of a cube face, unit directions in float, the radius folded into the frequencies) is SP-06's domain, for coarse maps
and previews; `evaluate(program, directions, out)` takes any list of unit directions. The same graph gives slightly
different values on the two domains: compare like with like.

**Cost.** One thread of an M5 Max evaluates SP-06's 11-octave terrain graph at 18 M samples/s on lattice chunks and
19 M samples/s on a cube face (`veach-bench-procgen`; ADR 0052's bar is 10 M). Streaming should run one chunk per
job; `evaluate(jobs, ...)` cuts one box into jobs of 4,096 samples, which only pays for large boxes.

**Bounds.** `pg::lattice_bounds(program)` proves, per output, the range of every value it can take, a Lipschitz
constant per realized meter and a bound on the interpreter's rounding, from the op list alone (interval arithmetic;
`veach:fbm@1`'s noise is bounded by 2 per unit amplitude and its gradient by 8.5·√3 per cycle). With one evaluation
at a box's center, `value(c) ± (lipschitz · radius + 2 · error)` bounds the whole box: terrain streaming uses it to
skip chunks that are all air or all rock ([voxel guide](voxel-terrain.md)). For a box of radius r,
`pg::lattice_variation(program, r)` is tighter: each octave moves by the smaller of its slope times r and its whole
range, so the fine octaves of a detail band stop counting once the box spans many of their wavelengths. The bounds
are conservative by design, so split boxes until they decide.

### The Luau builder

A builder script declares the same IR in strict Luau. It runs in the generator sandbox (same rules and budgets as
hooks) and goes through the JSON loader, so both routes give the same program and the same errors.

```lua
--!strict
return function(procgen)
	local g = procgen.graph("mymod:caves")
	local p = g:input("p", "p_local")
	local tunnels = g:node("tunnels", "veach:fbm@1", { p = p, wavelength = procgen.m(24), octaves = 4 })
	local rock = g:node("rock", "veach:fbm@1", { p = p, wavelength = procgen.m(3), octaves = 3, gain = 0.4 })
	local walls = g:node("walls", "veach:ridge@1", { x = tunnels, offset = 0.6 })
	g:output("density", g:node("density", "veach:add@1", { a = walls, b = rock }))
	return g
end
```

```cpp
veach::procgen::Sandbox sandbox;
auto graph = sandbox.build_graph(source, "caves.graph.luau");   // Result<FieldGraph>
```

`g:node(id, type, args)` puts every argument that is a value returned by `g:input` or `g:node` on the port of that
name and every other one in `params`; `procgen.planet_m(x)` and `procgen.m(x)` make lengths. The type annotations in
`tests/procgen/terrain.graph.luau` make it pass `luau-analyze` until a generator type environment exists (VCH-121).

## Generator hooks

A hook is a strict Luau chunk that returns `function(ctx, inputs, outputs)`, run in `veach::procgen::Sandbox`, a VM
separate from gameplay scripts ([ADR 0070](../adr/0070-generator-sandbox-rules.md)). Example:
`tests/scripts/sp07_climate.luau`, a climate and biome hook over a 64² × 6 cube map.

```cpp
veach::procgen::Sandbox sandbox;                                   // one per worker thread; not thread-safe
auto hook = sandbox.load(source, "climate.luau");
const veach::procgen::HookParam params[] = {{"obliquity", 0.41}};
const veach::procgen::HookInput inputs[] = {{"altitude", std::as_bytes(std::span(altitude))}};
const veach::procgen::HookOutput outputs[] = {{"biome", std::as_writable_bytes(std::span(biome))}};
auto ran = sandbox.run(*hook, {.seed = body_seed, .params = params, .inputs = inputs, .outputs = outputs});
```

What a hook may use, and the rules it must follow:

- `math` is canonical: every libm function is `veach::dmath`. `math.random`, `math.noise`, `os`, `print` and
  `require` are gone; randomness is `ctx.random(a, b, ...)`, a hash of the call's seed and the words you pass.
- `^` does not compile: write `math.pow(x, y)` or `x * x`.
- Inputs and outputs are `buffer`s; outputs are copied back only if the call succeeds.
- Nothing survives between calls: each runs in a fresh closure, so results never depend on call order.
- Never let output depend on `pairs` order over tables keyed by tables, functions or userdata. String and number
  keys are fine.
- Write integers in range into buffers; fractional or out-of-range values convert differently per platform.
- A hook that passes its safepoint or memory budget fails with `procgen.budget_exceeded`, even inside `pcall`. The
  safepoint budget is deterministic: what fits on one machine fits on all.
- Hooks run interpreted; never `--!native`.

Luau never runs per-sample loops over terrain: that is what field graphs are for.

## Stages

A stage is a pure C++ function from its declared inputs to one record ([ADR 0051](../adr/0051-staged-generation-pipeline.md)).
`space/gen/src/stages.cpp` registers the toolbox's G0, S1 and B1; the test pipeline in `tests/procgen_stage_tests.cpp`
is a smaller example.

```cpp
#include <veach/procgen/stage.hpp>
namespace pg = veach::procgen;

pg::StageRegistry registry;
auto universe = registry.add<Universe>({.id = "mymod:universe"}, &universe_hash,
    [](const pg::StageContext& ctx) { return Universe{ctx.universe_seed}; });
auto region = registry.add<Region>(
    {.id = "mymod:region", .version = 1, .stability = pg::Stability::pinned}, &region_hash,
    [](const pg::StageContext& ctx, const Universe& u) -> veach::Result<Region> {
        veach::HashStream rng = ctx.stream("layout");                 // seed: hash(universe, address, id, version)
        return make_region(ctx.address, u, rng);
    },
    pg::from(*universe, pg::root_address));                          // an ancestor, read at address 0

pg::StageRuntime runtime(registry, {.universe_seed = 42, .manifest_fingerprint = fp, .cache_capacity = 4096});
auto r = runtime.get(*region, address);                              // Result<std::shared_ptr<const Region>>
```

- **A stage reads only what it declared**: earlier stages, at its own address or an ancestor's (`AddressMap`). It
  has no world, no command buffer, no scale factor, and no randomness but its context's streams.
- **Registered by (id, version)**, with a stability class (`frozen`, `pinned`, `refreshable`,
  [ADR 0055](../adr/0055-universe-identity-and-versioning.md)) and a tier. Several versions of one id coexist; which
  one a body uses is the caller's choice (the toolbox's pins). A canonical stage cannot read a visual one.
- **A hash function per record**, built with `pg::RecordHasher` (fields in declaration order, doubles by their bits).
  The runtime keys each output by (stage, address, the hashes of its inputs, the manifest fingerprint), so
  `runtime.override(stage, address, record)` with an authored record re-runs exactly what depends on it.
- **Failures keep nothing**: a stage returning an error is `procgen.stage_failed`, naming the stage, its version and
  the address, and the next `get` runs it again.
- `get` is safe from many jobs at once. Run independent addresses in parallel with `jobs.parallel_for`, writing to
  indexed slots.

## Changing output

Canonical output is part of every universe ever made from it. To change what a node, a stage or a hook produces:

1. Add a new version (`veach:fbm@2`, `version = 2`) beside the old one; never change an existing version's output.
2. Commit the new version's hash in a unit test, checked on the macOS gate and on Windows CI.
3. Keep the old version and its hash until no universe uses it.

A test whose committed hash changes is a bug unless the change was a deliberate version bump.

## Adding a node type

Node types live in `engine/procgen/src/field_graph.cpp` (`node_types`, `parse_params`, the `compile` payload) and
`engine/procgen/src/field_eval.cpp` (the batch kernel in `run`). A node:

- takes only positions and other nodes' values, never a level or a chunk;
- uses only `+ − × ÷ √`, `floor`, `fma`, conversions, integer hashing and `veach::dmath` (ADR 0043);
- computes the products where its result cancels as one fused operation (`fused()`,
  [ADR 0071](../adr/0071-field-graph-precision-rules.md) rule 1), so a GPU evaluator can match it;
- gets a new opcode (the op list is what a GPU evaluator reads) and a test with a committed hash, batched and
  single-sample.

## Errors

`procgen.graph_invalid` (a malformed graph or a failing builder), `procgen.stage_invalid` (a stage that cannot be
registered), `procgen.stage_failed`, `procgen.budget_exceeded`, plus `data.parse_failed` and `data.unknown_field` for
JSON: [errors.md](../reference/errors.md).
