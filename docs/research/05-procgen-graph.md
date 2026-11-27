# 05: The procgen graph model

How procedural generation is described, executed, extended and edited in Veach: what runs in C++, on GPU compute or
in Luau; how creators add nodes, biomes, body kinds and phenomena; how scale-dependent and scale-independent features
are written down; and how all of it becomes the base of a nature editor. Q3 decides *which* stages the universe
pipeline has; this question decides *what a stage and a field are made of* and how they run. Raw notes and every
source: [notes/05-procgen-graph-notes.md](notes/05-procgen-graph-notes.md).

## 1. The problem in Veach's terms

Procgen in Veach has four workloads with very different shapes, and the mistake to avoid is forcing them into one
mechanism:

| Workload | Example | Count per body | Logic | Who depends on it |
|---|---|---|---|---|
| **Records** | star class, orbit, mass, radius, composition, retention, greenhouse, biome table | 1 to ~1,000 | branchy, physical formulas, non-local (Hill spacing, snow line) | gameplay, server, every later stage |
| **Coarse maps** | plates, drainage basins, rivers, climate fields, biome ids on a cube-sphere grid | ~10⁵-10⁶ samples, once | non-local (flow, erosion), iterative | terrain, biomes, weather |
| **Fields** | density or height at a point, cave carving, biome blend weights, material splat, scatter density | 10⁶-10⁹ samples, continuously while streaming | pure per-sample functions (noise, math, lookups into coarse maps) | collision (CPU), visuals (GPU) |
| **Scatter** | trees, rocks, resource nodes, grass, creature spawn points | 10³-10⁶ instances per streamed area | density → discrete points → filters | gameplay (some), visuals (most) |

Veach constraints that shape the answer:

- **Determinism is a contract** ([00-scope finding 4](00-scope.md), Q12). Anything gameplay or the server uses must come
  from a deterministic CPU path. Metal compiles with fast math on by default ([Apple docs](https://developer.apple.com/tutorials/data/documentation/metal/mtlcompileoptions/fastmathenabled.json)),
  and our build passes no floating-point mode to `slangc` or `xcrun metal` (`cmake/VeachShaders.cmake`).
- **Luau today** is strict, sandboxed, transactional and batched, with no `require` of user modules, no yield and no
  interrupt or timeout ([ADR 0007](../adr/0007-scripting-luau.md)); `math.sin` calls the platform libm
  ([lmathlib.cpp](https://raw.githubusercontent.com/luau-lang/luau/master/VM/src/lmathlib.cpp)). Native codegen is
  VCH-034.
- **Slang is compiled offline** ([ADR 0009](../adr/0009-shader-compilation.md)); runtime compilation is VCH-009. A
  data graph that a mod ships cannot become a new shader today.
- **Extension follows the material model** ([ADR 0035](../adr/0035-material-model.md)): override one function, call the
  public default from the override. Reflection feeds JSON, schemas, Luau types and MCP from one walker
  ([ADR 0026](../adr/0026-reflection-everywhere.md)). Text everywhere.
- **Layering** ([ADR 0042](../adr/0042-space-toolbox-layering.md)) already names "a procgen graph runtime" as a
  generic need that moves down into `engine/`. Planet-specific stages and kinds live in `space/`.
- **Headless first.** The null RHI does nothing, so every authoritative result, every test and every MCP preview must
  work with no GPU.
- **Scale is a creator setting** (s_p, s_sys from [00-scope](00-scope.md)). The pipeline runs on physical values;
  planet-scale features follow s_p, human-scale detail stays in meters.
- **Single-threaded today**; the job system is VCH-015. Coarse stages and CPU field evaluation need workers.

The (a) items 00-scope gives Q5: stage hooks in Luau and C++; deterministic math for hooks; GPU output visual-only by
default; coarse cached stages next to chunk-local ones; body kinds and biomes as registries, not enums; a hook cannot
read data from a later stage; the graph is editable data so a nature editor can come later (b).

## 2. Options, with precedents

### A. One universal node graph for everything

Every stage, from star mass to grass blades, is a node in one graph family (Houdini, Unreal PCG, Gaea, World
Machine style).

- **Unreal PCG**: graphs of point/spatial data; custom nodes are a `UPCGSettings` plus an `IPCGElement` whose
  `ExecuteInternal` can return false to time-slice ([UE API](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/PCG/IPCGElement?lang=en-US));
  hierarchical grids cascade data from large cells to small, never back, with an "Unbounded" grid that runs once
  ([UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-generation-modes-in-unreal-engine));
  GPU nodes are grouped into compute graphs, CPU/GPU transfers are visible badges, and **GPU-spawned instances get no
  collision, navigation or HLOD** ([UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-with-gpu-processing-in-unreal-engine)).
- **Houdini**: VOP networks compile to VEX, run per point in SIMD batches, "close to compiled C/C++"
  ([SideFX](https://www.sidefx.com/docs/houdini/vex/index.html)); Far Cry 5 used Houdini tools for biomes, rivers,
  cliffs and power lines over "100 square km of wilderness with a terrain changing every day"
  ([GDC Vault](https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far), [80.lv](https://80.lv/articles/houdini-procedural-world-generation-of-far-cry-5/)),
  offline (UNVERIFIED detail).
- **Gaea / World Machine**: global erosion nodes seam when tiled, change with resolution, and in Gaea "may differ
  slightly each time" ([Gaea](https://docs.quadspinner.com/Reference/Erosion/Erosion.html),
  [World Machine forum](https://forum.world-machine.com/t/how-to-mitigate-resolution-dependencies/6424)).

Fits fields and scatter well. Fits records badly: Kepler orbits, Hill spacing and a greenhouse model are fifty lines
of code and two hundred nodes. Global nodes inside a per-sample graph are exactly what breaks tiling and determinism
in Gaea and World Machine.

### B. Code-first generators with registries, no graph

Stages are C++ functions (and Luau functions) registered by id; noise is hand-written, once in C++ for the CPU and
once in Slang for the GPU. This is how Elite's Stellar Forge reads from outside: a top-down simulation with per-planet
properties "packed up into a buffer which is sent to the GPU" and terrain made by compute shaders, plus separately
generated physics patches, with native-double and emulated two-float noise versions maintained side by side
([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)). No Man's Sky generates voxels, meshes,
textures, populates and simulates continuously at runtime
([gamedeveloper.com](https://www.gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-));
that its generation is C++ code tuned by data files is UNVERIFIED.

Fits records perfectly. For fields it means two implementations of every terrain function that must agree, which is
the drift hobbyist planet projects keep reporting
([three.js](https://discourse.threejs.org/t/space-person-multiple-planets-clouds-biomes-flying-and-walking/90091.md),
[Godot](https://forum.godotengine.org/t/help-with-procedural-planets/139951), anecdotal). A mod can't add a terrain
shape without C++ and Slang, and a nature editor has nothing to edit.

### C. Script-declared expression graphs (the Factorio model)

Scripts don't evaluate fields; they *declare* them. Factorio mods write noise expressions as Lua data; C++ parses
them, folds constants once map settings are known, deduplicates by hash (31,000 → 5,300 objects), and compiles one
multi-output "noise program" per surface that runs per chunk, multithreaded and SIMD. That work took Vulcanus chunk
generation from 18.35 ms to 2.83 ms, and the team called its noise visualizer "invaluable"
([FFF #390](https://factorio.com/blog/post/fff-390), [FFF #258](https://factorio.com/blog/post/fff-258)).

Fits fields, gives mods real power with no native code, and keeps scripts out of the hot loop. On its own it has no
answer for records or coarse non-local maps, and an expression that lives only inside a script has no stable text
form for an editor to load and save.

### D. Hybrid: code-first stages, data-first fields and scatter (recommended)

Two mechanisms, each used where it fits:

1. A **stage pipeline** for records and coarse maps. Stages are code (C++ by default, Luau allowed), registered by id,
   versioned, with typed, reflected inputs and outputs, run on CPU workers and cached by content hash. Precedents:
   Stellar Forge's top-down stages; Unreal PCG's one-way grid cascade; Gaea's advice to bake non-tiling stages at a
   fixed full-world resolution ([Gaea](https://docs.gaea.app/guides/scenarios/large-projects/resolution-tiles-without-seams.html)).
2. A **field graph** for per-sample functions and scatter. It is a DAG of typed nodes stored as JSON (the canonical
   form), which Luau can also build through a builder API (option C folded in). It runs on two evaluators: a **CPU
   batch interpreter** (authoritative) and a **GPU evaluator** (visual). Precedents: Horizon Zero Dawn compiled one
   artist graph to an intermediate form that ran as GPU placement and *also* in an in-game GPU interpreter for
   stepping through layers ([slides](https://www.slideshare.net/slideshow/gpubased-procedural-placement-in-horizon-zero-dawn/72897079));
   FastNoise2 is a SIMD node tree serialized to a string, with a node editor that pushes live updates to a running
   app ([FastNoise2](https://github.com/Auburn/FastNoise2)); Blender evaluates fields in batches through
   multi-functions so per-node overhead is paid per batch ([T98940](https://developer.blender.org/T98940)).

**How the field graph executes** (sub-options inside D):

| | D1: interpreters (recommended first) | D2: generated Slang, offline | D3: generated Slang at run time | D4: per-sample Luau |
|---|---|---|---|---|
| CPU | batch interpreter: each node processes a SIMD batch of positions (FastNoise2/Blender style) | same | same | Luau loop per sample |
| GPU | a Slang interpreter walking an op list in a buffer; all threads run the same program, so no divergence | the graph becomes a Slang module at build time | the graph becomes a Slang module, linked against precompiled node modules with link-time types ([Slang](https://shader-slang.org/slang/user-guide/link-time-specialization.html)) | none |
| Edit → result | instant (upload new op list) | rebuild | a compile on every edit (latency unmeasured) | instant |
| Mod-shipped graphs | yes | no | yes | yes |
| Needs | nothing new in the RHI | nothing | VCH-009 (libslang at run time) | nothing |
| Speed | interpreter overhead on GPU unmeasured (spike S1) | best | best | far too slow for chunks (S2 measures) |

D1 now, with D3 added only if S1 measures the GPU interpreter at more than 2× a hand-written kernel on a real graph.
D2 stays available for engine-internal graphs that never change at run time.

## 3. Tradeoffs

| | A. Universal graph | B. Code-first only | C. Script-declared fields only | **D. Hybrid** |
|---|---|---|---|---|
| **Complexity** | High: a graph language that must also express physics, loops and global operators | Low to start, grows with every GPU/CPU duplicate | Medium: an expression compiler | Medium-high: a stage scheduler with a cache, plus a field IR with two evaluators |
| **Performance** | Good for fields; poor for records (node overhead on branchy logic) | Best per function, paid twice | Good (SIMD, folded, deduped) | Fields near-native via batching (FastNoise2 reports 261 M points/s for 3D Perlin on one AVX2 core); records in native code |
| **Determinism** | Global nodes are the classic failure (Gaea) | Two implementations drift | One definition; the CPU path can be made deterministic | One definition; the CPU path is the authority, so the GPU may differ in ulps |
| **Extensibility** | Nodes only | C++ only for anything per-sample | Expressions from scripts; no stages | Stages from Luau or C++; nodes from C++ modules; graphs from JSON or Luau; kinds as data |
| **Testability** | Graph diffs are hard to read | Unit tests per function | Expressions are text | IR is text with stable node ids; CPU evaluator runs headless; GPU/CPU parity is one test |
| **Scale range** | Annotation possible but ad hoc | Every function decides by hand | Possible in the expression types | Units are part of the IR's type system (section 5) |

Specific costs inside D worth stating plainly:

- **Two evaluators to keep in step.** Every node type ships a C++ batch kernel and a Slang function. The parity test
  (same graph, both evaluators, tolerance on the result) is the price, and it is cheaper than a hand-written duplicate
  of every terrain function.
- **The GPU result is not bit-equal to the CPU result.** Integer lattice hashing (PCG family, [Jarzynski & Olano 2020](https://www.jcgt.org/published/0009/03/02/paper.pdf))
  is bit-exact everywhere, but gradients and interpolation are floats, and float shader hashes diverge between CPU and
  GPU ([danilw](https://danilw.github.io/blog/Hash_Noise_in_GPU_Shaders)). So the rule stays: **GPU output is
  visual-only.** Near the player, the CPU mesh built for collision is also the one drawn (Q4), so what you see is what
  you stand on. Far away, a sub-millimetre difference is invisible.
- **The interpreter's GPU overhead is the main unknown.** HZD shipped an interpreter only for debugging; using one as
  the production path is our bet, and S1 measures it.

## 4. The design in more detail

### 4.1 Execution tiers

| Tier | Runs | Where | Authority | Cached |
|---|---|---|---|---|
| **Stages** (records) | C++ stage functions; Luau overrides and hooks | CPU worker jobs (VCH-015); main thread until then | authoritative | yes: key = (id, stage id, stage version, input hash, manifest hash) |
| **Coarse maps** | C++ stages (flow, plates, climate); Luau allowed for low-res maps (≤ ~64²×6, S2 measures) | CPU workers | authoritative | yes, per body; persisted optionally |
| **Fields** | field graph, CPU batch interpreter | CPU workers, near entities that need collision or queries (Elite's "physics patch for any entity that requires it") | authoritative | per chunk, by graph hash |
| **Fields** | field graph, GPU evaluator | compute passes in the render graph | visual-only | GPU-side, by graph hash |
| **Scatter, authoritative** | field graph density → deterministic discretization on CPU (ids from cell + index) | CPU | authoritative: trees with collision, resource nodes, spawn points | per chunk |
| **Scatter, visual** | same density nodes → GPU discretization (HZD's ordered dithering) | GPU, needs indirect draw (VCH-071) | visual-only: grass, pebbles, debris | none (regenerated) |
| **Rules and content** | Luau and JSON: biome kinds, body kinds, phenomena, suitability rules, species lists | load time, plus stage invocations | authoritative | by content hash in the manifest |

What gameplay needs on the CPU (and so never comes from the GPU): the surface and caves near any physics body, ground
height and normal at a point, biome and water at a point, authoritative scatter (anything with collision, anything
harvested, anything synced), atmosphere and body records, and every stage record. The server needs exactly this set
and runs with `--rhi null`.

### 4.2 Stages

A stage is registered with an id (`core:climate`), a version, its input record types (earlier stages only, so a hook
can't read a later stage, as 00-scope requires), its output record type (a reflected struct, so it is printable,
JSON-serializable and visible over MCP), a named `veach::Rng` stream derived from the parent id, and an
implementation. The pipeline is a DAG per body kind, resolved at load time; a cycle or a read from a later stage is
an error with a code and a hint.

**Override one stage, like a material overrides one function.** A body kind names its stages, and each has a default:

```json
{ "id": "mymod:tidally_locked_rock", "extends": "core:rocky",
  "stages": { "core:climate": "mymod:climate_locked" } }
```

```lua
--!strict
local procgen = require("@veach").procgen
procgen.stage("mymod:climate_locked", { version = 1, extends = "core:climate" },
	function(ctx, body, default)
		local climate = default(ctx, body)          -- the public default, like standardEmission(c)
		climate.terminator_width = 0.1 * body.radius_m
		return climate
	end)
```

Stage functions are **pure**: inputs in, a record out. They get no world access and no command buffer, so they can
run on any worker, be cached, be replayed and be re-run after a version bump. An error fails that stage for that id
with `procgen.stage_failed`; nothing partial is kept, which is the transactional rule in another form.

### 4.3 The field graph IR

JSON is canonical; Luau builds the same IR. A sketch:

```json
{
  "veach_graph": 1,
  "id": "core:terrain/rocky",
  "inputs": ["p_planet", "p_local", "coarse.elevation", "coarse.biome"],
  "params": {
    "continent_wavelength": { "value": 2.0e6, "unit": "planet_m" },
    "rock_wavelength":      { "value": 3.0,   "unit": "m" }
  },
  "nodes": {
    "continents": { "type": "veach:fbm@1", "p": "$p_planet", "wavelength": "@continent_wavelength", "octaves": 6 },
    "rocks":      { "type": "veach:ridged@1", "p": "$p_local", "wavelength": "@rock_wavelength", "octaves": 3 },
    "height":     { "type": "veach:sum@1", "terms": [
                      { "of": "continents", "amplitude": { "value": 3000, "unit": "planet_m" } },
                      { "of": "rocks",      "amplitude": { "value": 0.4,  "unit": "m" } } ] }
  },
  "outputs": { "height": "height" }
}
```

```lua
local g = procgen.graph("mymod:terrain/canyons")
local continents = g.fbm({ p = g.p_planet, wavelength = g.planet_m(2e6), octaves = 6 })
local rocks = g.ridged({ p = g.p_local, wavelength = g.m(3), octaves = 3 })
g:output("height", continents * g.planet_m(3000) + rocks * g.m(0.4))
```

- **Node types are versioned** (`veach:fbm@1`). Changing a node's output for the same inputs is a new version; old
  versions stay in the binary while any universe uses them (Q11).
- **Node ids are stable strings**, and each node's seed is `hash(body seed, node id)`, so adding a node does not
  reshuffle the others (Unreal's per-point seeds and HZD's stable ids serve the same purpose).
- **Node parameters are reflected structs**, so JSON Schemas, Luau builder types and MCP descriptions are generated
  ([ADR 0026](../adr/0026-reflection-everywhere.md)). A typo gets a did-you-mean.
- **Graph-level optimization** (constant folding, dedup, one multi-output program per graph) follows Factorio.
- **Coarse maps are inputs**, sampled through a node (`veach:sample_cubemap@1`), so non-local results reach the
  per-sample world without per-sample global work. They live in storage buffers, not textures, so VCH-076 is not on
  the critical path.
- **Positions arrive split**: an integer lattice cell plus a float offset, computed in double on the CPU per chunk
  (Elite needed double or two-float noise for the same reason). Hashing integer cells is exact at any radius, from an
  asteroid to a 70,000 km gas giant.

### 4.4 Execution in the engine

`engine/procgen` (a new engine area; generic per ADR 0042) owns: the IR, the JSON loader and the Luau builder, the node
registry, the stage scheduler and cache, the CPU batch interpreter, and the GPU evaluator as a library that render
features call to add compute passes to the render graph. Field programs are assets (handles, not GPU objects in
components), so the extraction boundary holds. `space/` registers the toolbox's stages (star, orbit, body, climate,
biome), nodes (craters, plate distance) and kinds.

Headless and MCP: with `--rhi null` the GPU evaluator is absent and everything authoritative still runs. Proposed MCP
tools (a spec later): `procgen_inspect {id}` returns the stage records for a body; `procgen_eval {graph, node,
positions}` evaluates any node, not only the outputs, on the CPU; `procgen_preview {graph, node, region,
resolution}` renders a slice or a lat/long map *on the CPU* to a PNG, so previews work headless and in CI.

## 5. Scale: how features say what they scale with

Every length-valued parameter and amplitude in the IR has a **unit class**, and the compiler refuses a bare number:

- `planet_m`: physical meters at s = 1. Geometry applies s_p. Continents, mountain ranges, basins, plate
  boundaries, biome patches, storm cells.
- `m`: meters, never scaled. Rocks, cave tunnels, river banks, pebbles, edit brushes, scatter footprints.
- `system_m` exists for system-scale stages (orbits), which use s_sys.

Two position inputs make the split concrete: `p_planet` is the unscaled body-fixed position (direction × physical
radius, plus altitude ÷ s_p); `p_local` is meters on the scaled surface. Planet-unit nodes sample `p_planet`, so
**the same seed gives the same continents at the same latitude and longitude at any s**; meter-unit nodes sample
`p_local`, so a rock is the same size at any s. Radial displacement = s_p · h_planet + h_m.

What the compiler does with the annotation:

- **Band floor.** At small s, planet-unit wavelengths shrink into the meter band (2 km hills at s = 0.01 are 20 m
  hills). The compiler knows each octave's effective wavelength in meters and fades out planet octaves below a
  creator floor (`min_planet_wavelength_m`, the same idea as 00-scope's biome-patch floor). It warns when two bands
  overlap.
- **LOD band limiting.** The evaluator drops octaves whose wavelength is below twice the sample spacing. That is
  anti-aliasing for far LODs and the main quality-tier knob (section 7), and it comes for free from the units.
- **Relief.** With `planet_m` amplitudes, slopes are invariant under s, but an 8.8 km mountain becomes 88 m at
  s = 0.01. Compressed-scale games usually exaggerate relief; KSP's Kerbin keeps mountains of several km on a
  ~600 km radius (UNVERIFIED figures). So planet amplitudes get one creator setting, `relief_exponent` α: amplitude
  scales as s_p^α instead of s_p. It is a product decision (section 12).
- **Validation.** The graph tester evaluates every graph at the supported s extremes and fails on bands that
  overlap after the floor, or on NaN or infinity.

Scale-independent quantities (temperature, composition, biome identity) are not lengths and carry no unit class:
they come from stages on physical values.

## 6. Extension points

| What | Luau (scripts and data) | C++ module | Toolbox core only |
|---|---|---|---|
| New **field graph** (terrain shape, cave style, splat) | yes: JSON or the Luau builder, composing existing nodes; composite nodes (subgraphs) as reusable nodes | yes | — |
| New **node type** (a new noise, an SDF primitive, a crater) | no per-sample code (too slow, non-deterministic libm); composite nodes only | yes: a C++ batch kernel + a Slang function + parameter struct, registered by id and version | — |
| New **stage** or **stage override** | yes: pure function, deterministic math, budgeted | yes | the default pipeline order for `core:` kinds |
| **Body kind** (lava world, ring-world fragment) | yes: JSON (stages, graphs, render feature names) + optional Luau stages | yes, when it needs a new terrain representation (no surface, shells) | the representation set itself (Q4) |
| **Biome kind** | yes: JSON climate envelope, palette, graphs, scatter species; optional Luau `suitability` rule | yes | — |
| **Phenomenon** (geysers, aurorae, eruptions) | placement rules (scatter graph or stage), schedule and parameters | rendering (render features are C++), simulation behaviour as a module (Q7) | — |
| **Scatter species** (flora, rocks) | yes: data + density graph | procedural plant models (later) | — |
| **Evaluator backends** (D3 compiled Slang, a new SIMD target) | — | — | engine |

**Mods change universe identity** (Q11): every registered graph, stage, kind and node contributes its id, version and
content hash to the universe manifest. Same seed + different manifest = different universe; a client with a
different manifest is refused with a list of what differs.

**The Luau procgen context** (an engine change; needs VCH-034's interrupt for budgets):

- A separate sandboxed VM per worker thread (a `lua_State` is not shared across threads).
- `math` is replaced with a deterministic implementation under the same names. `math.random`, `math.noise`
  (float32 Perlin with a 256 period, [lmathlib.cpp](https://raw.githubusercontent.com/luau-lang/luau/master/VM/src/lmathlib.cpp))
  and `os.clock` are removed. Randomness comes only from `ctx.rng`.
- `require` of the mod's own modules, read-only.
- `buffer` for coarse maps ([Luau buffer](https://luau.org/library)), since script components can't hold arrays.
- An instruction or time budget per invocation through `lua_Callbacks::interrupt`
  ([Luau sandbox](https://luau.org/sandbox)), failing with a code.
- `--!native` allowed for hooks once VCH-034 lands. Codegen specializes on type annotations
  ([luau.org](https://luau.org/performance)), so hooks should be annotated.

## 7. Interactions with the other questions

- **Q1 coordinates:** fields take (integer cell, float offset) positions in the body-fixed frame; Luau needs a
  precise position type for hooks that place things. Chunk origins come from Q1's frame tree.
- **Q2 mechanics and time:** orbits and rotation are stage records; Q2's solver is called by stages, not graphs.
  Seasons feed climate stages.
- **Q3 pipeline:** Q3 owns the list and order of stages and the seed hierarchy; Q5 owns the stage contract (pure,
  typed, versioned, cached, overridable) and the field IR. A conflict to settle in the synthesis: Q3 may want
  galaxy-scale density sampling (star placement) as a field. It fits the IR with `system_m`-like galaxy units.
- **Q4 terrain:** the density function *is* a field graph output. Caves that are networks (non-local) are a coarse
  stage producing tunnel splines and a field node "distance to tunnels" (the coarse-plus-local pattern again). The CPU
  mesh near entities is both collision and draw. Player edits apply after the graph, as deltas (Q11). Q4's
  chunk-ready hook is where authoritative scatter runs.
- **Q6 networking:** only CPU-authoritative outputs may be "regenerated instead of sent". The manifest handshake
  includes graph and node hashes.
- **Q7 simulation LOD:** ecosystems read biome records and authoritative scatter; eruptions are system edits placed
  by phenomenon rules.
- **Q8 renderer:** GPU field evaluation and visual scatter are compute passes inside render features; material splat
  comes from biome-weight outputs; vegetation needs indirect draw (VCH-071).
- **Q9 physics:** colliders come from CPU field evaluation around physics bodies, never from GPU readback.
- **Q10 travel:** approach speed sets how many chunks per second the GPU evaluator (visual) and the CPU evaluator
  (near the landing site only) must produce.
- **Q11 persistence:** node and stage versions, content hashes, the manifest. Old node versions are kept runnable;
  this is the most expensive commitment in this document.
- **Q12 determinism:** the CPU evaluator and Luau hooks are authoritative code paths: `-ffp-contract=off` on Clang and
  the MSVC equivalent, no approximate reciprocals or rsqrt, engine-provided transcendentals, integer lattice hashing,
  and a CI test that hashes stage and field outputs on macOS arm64 and Windows x64. FastNoise2's README warns that MSVC
  has SIMD bugs producing wrong output ([FastNoise2](https://github.com/Auburn/FastNoise2)), which is one more reason to
  run that test in CI. Slang and Metal floating-point modes should be pinned explicitly even for visual-only work, so
  visuals don't change with a driver default.

## 8. M1-class vs M3 Pro-class

Hardware facts: M3 Pro has ~150 GB/s of memory bandwidth ([AnandTech](https://anandtech.com/show/21116),
[MacRumors](https://www.macrumors.com/2023/10/31/apple-m3-pro-less-memory-bandwidth/)). A base M1 has an 8-core GPU,
~68 GB/s and 4 performance + 4 efficiency CPU cores (UNVERIFIED in this pass). M3 Pro has up to 18 GPU cores and up to
12 CPU cores (UNVERIFIED). Only the M5 Max can be measured; everything below is derived and must be checked by the
spikes.

| Concern | Base M1-class | M3 Pro-class |
|---|---|---|
| CPU authoritative fields | ~3 worker P-cores. If one NEON core reaches half of FastNoise2's AVX2 figure, a ~10-noise density graph is ~10 M samples/s/core, ~3.6 ms per 33³ chunk per core (derived, UNVERIFIED). Enough for collision around a few players and a landing site, not for meshing what the camera sees. | Roughly twice the worker cores: room for a listen server with several players on different bodies. |
| GPU visual fields | One queue (no VCH-074), so generation shares the frame. A procgen compute budget must be set per tier and time-sliced. | Same queue limit, more ALU: larger budget, finer far LODs. |
| Visual scatter | Bandwidth-bound: lower grass density and shorter scatter radius. | ~2.2× the bandwidth. |
| Coarse stages, Luau hooks | Once per body, single-threaded per stage; fine on both if each body's stages fit a per-body budget measured in S2. | Same. |
| Quality knob | LOD band limiting (section 5) removes octaves by sample spacing; per-tier spacing multipliers and scatter radius. | Same knobs, higher defaults. |

RHI gaps per option:

- **D1** needs nothing new for fields in buffers.
- **Visual scatter** needs VCH-071 (indirect draw, and indirect dispatch for variable counts).
- **Density bricks and clouds as volumes** want VCH-072. Buffers work first.
- **Overlap with the frame** wants VCH-074. Without it, generation is time-sliced inside the graph.
- **VCH-073** (non-blocking readback) is not needed, by design: no gameplay result comes back from the GPU. Parity tests
  may block.
- **VCH-076** is avoided by keeping coarse maps in buffers.
- **D3** adds VCH-009.
- **Options A and B** carry the same RHI needs; B adds nothing, but pays in duplication instead.

## 9. Recommendation, and what would change my mind

**Adopt D with D1:**

- A code-first stage pipeline for records and coarse maps: pure, typed, versioned, cached, overridable like
  materials.
- A data-first field graph for per-sample work and scatter: JSON IR with stable node ids, unit-typed parameters and
  versioned node types, buildable from Luau.
- The field graph runs on a CPU SIMD batch interpreter, which is authoritative and headless, and a GPU interpreter,
  which is visual-only.
- Luau declares graphs, rules and content and overrides stages. It never runs per-sample loops.
- The runtime lives in `engine/procgen`; planets live in `space/`.

What would change my mind:

- **S1 shows the GPU interpreter at more than 2× a hand-written kernel.** Then move GPU evaluation to D3 (runtime Slang
  with precompiled node modules), which pulls VCH-009 forward. The IR doesn't change.
- **S1 shows the CPU interpreter well below 10 M samples/s/core for a realistic graph.** Then add a CPU code generator
  for engine-shipped graphs (D2-style C++ output) and keep the interpreter for mods.
- **S2 shows Luau hooks are too slow for per-body coarse maps even with `--!native`.** Then restrict Luau to records
  and push coarse maps to graph nodes and C++.
- **S2 shows Luau's double arithmetic is not bit-equal between arm64 and x64,** for example through codegen
  contraction. Then Luau hooks become non-authoritative, and only record-level, integer-heavy logic stays
  authoritative.
- **The owner wants mods to ship per-sample GPU code.** Then D3 and a sandboxed Slang node API become founding work,
  not an optimization.

## 10. What is expensive to undo

1. **Node semantics and versions.** Once universes and saves depend on `veach:fbm@1`, that exact function must stay
   runnable forever (or until those universes are migrated). Choose the hash, the gradient set and the interpolation
   carefully before anything ships.
2. **The deterministic math library and the hash.** Changing a polynomial for `sin` changes every universe that
   used it.
3. **"GPU is visual-only".** If gameplay ever starts reading GPU results, determinism and headless servers are gone.
   Hold the line from the first spike.
4. **The unit classes and the s mapping** (`planet_m`, `m`, `p_planet`, `p_local`). Every graph is written against
   them.
5. **The stage contract** (pure, typed records, no later-stage reads). Relaxing it later breaks caching and replay.
6. **Registry id format and mod namespacing** (`mod:name`). It goes into manifests, saves and wire formats.
7. **JSON IR shape.** It is cheaper to change than the rest (a migration tool can rewrite text), but every mod and
   every editor file depends on it.

Cheap to change: interpreter internals, D1 → D3, the Luau builder's surface, caching policy, the MCP tool set.

## 11. Proposed ADRs

- *Procgen is a pipeline of code-first stages plus data-defined field graphs.*
- *The procgen runtime is an engine area; planetary stages and kinds live in the toolbox.*
- *Procgen stages are pure, typed, versioned and cached, and are overridden like materials.*
- *Field graph IR: JSON, stable node ids, versioned node types, Luau builder.*
- *Field graphs run on an authoritative CPU batch interpreter and a visual-only GPU evaluator.*
- *Procgen units: planet meters, meters and system meters, with a band floor.*
- *The Luau procgen context: deterministic math, no world access, budgets, per-worker VMs.*
- *Procgen registries with mod-namespaced ids feed the universe manifest* (jointly with Q11).

## 12. Spike candidates

Each is a throwaway prototype, sized by scope.

**S1: one graph, two evaluators.**
- Scope: a JSON IR with ~10 node types: position inputs, PCG-hash gradient noise, fbm, ridged, domain warp, remap,
  add/mul, clamp and curve. Plus a CPU batch interpreter (scalar and one SIMD path), a Slang GPU interpreter over an
  op list in a storage buffer, and the same graph hand-written in Slang.
- Visible success: three height-map images side by side (CPU, GPU interpreter, hand-written) with a max-abs-error
  readout under 1e-5 of the range, on Metal and on WARP.
- Measured success: Tracy CPU ns/sample/core and GPU ms per 10⁶ samples for interpreter vs hand-written, on the M5
  Max. The target is the GPU interpreter within 2× of hand-written and the CPU at ≥ 10 M samples/s/core.
- The CPU half runs under `--rhi null` in CI.

**S2: the Luau procgen context.**
- Scope: a sandboxed per-worker VM with deterministic `math` and an interrupt budget, plus one pure stage that
  computes a climate record and assigns biome ids over a 64²×6 cube-sphere map held in a `buffer`, run interpreted and
  with `--!native`.
- Success: identical output hashes on the macOS arm64 gate and Windows x64 CI; ms per body logged for both modes; a
  hook with an infinite loop stops with an error code and the app exits 1.

**S3: scale annotations by eye.**
- Scope: one rocky-planet graph with `planet_m` continents and mountains and `m` rocks, evaluated by the CPU
  evaluator headless at s = 1 and s = 0.01, with the band floor and two `relief_exponent` values.
- Visible success: equirectangular maps that match at both scales; 100 m close-ups where the rocks are the same size;
  relief variants side by side for the owner to judge.

**S4: the live edit loop.**
- Scope: the graph as an asset with hot reload, a cache keyed by graph hash, and a `procgen_preview` MCP tool. A
  prototype can go through `eval` plus a written PNG.
- Success: an agent edits one parameter in the JSON, calls `reload`, and the preview PNG changes; stale cache
  entries are not reused; it works with `--rhi null`.

S1 comes first: its numbers decide D1 vs D3. S2 depends on VCH-034's interrupt work, or a stub of it.

## 13. Product decisions surfaced

1. **May mods ship per-sample GPU code (custom Slang nodes)?** Recommendation: not at first. C++ modules add nodes;
   data-and-Luau mods compose nodes and override stages. Revisit after VCH-009, with the CPU/GPU parity test as the
   entry gate.
2. **Is a modded universe a different universe?** Recommendation: yes. The manifest (seed, generator versions, mod
   hashes, scale) is the universe's identity, and multiplayer requires identical manifests.
3. **Relief under compressed scale.** Recommendation: make `relief_exponent` a per-universe creator setting, with a
   default of 0.5 to confirm by eye in S3. At 0.5, an 8.8 km mountain becomes 880 m at s = 0.01.
4. **Who is the nature editor for?** Recommendation: a creator and developer tool first: the data model, MCP tools
   and CPU previews now, and a UI later as a client of the same API. Not an in-game player tool in the first versions.

## Sources

Graph tools:
- Unreal PCG: [IPCGElement API](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/PCG/IPCGElement?lang=en-US),
  [generation modes](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-generation-modes-in-unreal-engine),
  [GPU processing](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-with-gpu-processing-in-unreal-engine),
  [biome core reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-pcg-biome-core-and-sample-plugins-reference-guide-in-unreal-engine),
  [custom nodes tutorial](https://blueshift-interactive.com/2025/09/03/how-to-create-custom-pcg-nodes/)
- Blender: [lazy-function commit](https://developer.blender.org/rB4130f1e), [3.0 notes](https://developer.blender.org/docs/release_notes/3.0/nodes_physics/),
  [T98940](https://developer.blender.org/T98940), [T90875](https://developer.blender.org/T90875), [T98492](https://developer.blender.org/T98492)
- Houdini: [VEX](https://www.sidefx.com/docs/houdini/vex/index.html), [VEX SOP](https://www.sidefx.com/docs/houdini/nodes/sop/vex.html),
  [TOPs](https://www.sidefx.com/docs/houdini/tops/intro.html), [Houdini Engine PDG](https://www.sidefx.com/docs/houdini/unreal/pdg.html)
- Gaea: [erosion](https://docs.quadspinner.com/Reference/Erosion/Erosion.html), [build types](https://docs.quadspinner.com/Guide/Build/Build-Types.html),
  [tiled builds](https://docs.gaea.app/using/using-gaea/build-and-export/tiled-builds.html),
  [resolution without seams](https://docs.gaea.app/guides/scenarios/large-projects/resolution-tiles-without-seams.html)
- World Machine: [tiling forum](https://forum.world-machine.com/t/single-vs-tiled-significant-render-differences/4408),
  [resolution forum](https://forum.world-machine.com/t/how-to-mitigate-resolution-dependencies/6424)
- Unity: [Shader Graph generated code (third party)](https://www.cyanilux.com/tutorials/intro-to-shader-graph/),
  [VFX Graph](https://unity.com/glossary/vfx-graph),
  [Graph Toolkit](https://discussions.unity.com/t/unity-s-graph-toolkit-experimental-available-today-in-unity-6-2/1664909)
- FastNoise2: [README](https://github.com/Auburn/FastNoise2)
- Factorio: [FFF #390](https://factorio.com/blog/post/fff-390), [FFF #258](https://factorio.com/blog/post/fff-258),
  [noise expressions API](https://lua-api.factorio.com/latest/auxiliary/noise-expressions.html)

Games:
- Horizon Zero Dawn, GDC 2017: [slides](https://www.slideshare.net/slideshow/gpubased-procedural-placement-in-horizon-zero-dawn/72897079),
  [GDC Vault](https://www.gdcvault.com/play/1024120/GPU-Based-Run-Time-Procedural),
  [PDF](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf)
- Far Cry 5, GDC 2018: [GDC Vault](https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far),
  [80.lv](https://80.lv/articles/houdini-procedural-world-generation-of-far-cry-5/),
  [PlayStation blog](https://blog.playstation.com/2018/03/22/the-procedural-world-generation-of-far-cry-5/)
- No Man's Sky, GDC 2017: [GDC Vault](https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No),
  [gamedeveloper.com](https://www.gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-),
  [uber noise (secondary)](https://github.com/gistya/nomansterrain)
- Elite Dangerous: [80.lv interview](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)
- Dreams, SIGGRAPH 2015: [slides](https://advances.realtimerendering.com/s2015/AlexEvans_SIGGRAPH-2015-sml.pdf) (not read)
- Anecdotal CPU/GPU mismatch: [Godot forum](https://forum.godotengine.org/t/help-with-procedural-planets/139951),
  [three.js forum](https://discourse.threejs.org/t/space-person-multiple-planets-clouds-biomes-flying-and-walking/90091.md)

Execution and determinism:
- Luau: [performance](https://luau.org/performance), [library](https://luau.org/library), [sandbox](https://luau.org/sandbox),
  [lmathlib.cpp](https://raw.githubusercontent.com/luau-lang/luau/master/VM/src/lmathlib.cpp),
  [recap July 2024](https://luau.org/2024/07/23/luau-recap-july-2024.html),
  [Roblox native codegen](https://create.roblox.com/docs/luau/native-code-gen),
  [timeouts discussion](https://github.com/luau-lang/luau/discussions/239)
- Slang: [link-time specialization](https://shader-slang.org/slang/user-guide/link-time-specialization.html),
  [compiling](https://shader-slang.org/slang/user-guide/compiling.html)
- Metal: [fastMathEnabled](https://developer.apple.com/tutorials/data/documentation/metal/mtlcompileoptions/fastmathenabled.json)
- Hashing: [Jarzynski & Olano 2020](https://www.jcgt.org/published/0009/03/02/paper.pdf),
  [Reed](https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering),
  [danilw](https://danilw.github.io/blog/Hash_Noise_in_GPU_Shaders),
  [Intel forum](https://community.intel.com/t5/OpenCL/GPU-and-CPU-floating-point-equivalence/m-p/1084936)

Hardware:
- [AnandTech M3 family](https://anandtech.com/show/21116), [MacRumors M3 Pro bandwidth](https://www.macrumors.com/2023/10/31/apple-m3-pro-less-memory-bandwidth/),
  [Apple newsroom](https://www.apple.com/newsroom/2023/10/apple-unveils-m3-m3-pro-and-m3-max-the-most-advanced-chips-for-a-personal-computer/)

Repository: [ADR 0007](../adr/0007-scripting-luau.md), [0008](../adr/0008-shaders-slang.md),
[0009](../adr/0009-shader-compilation.md), [0026](../adr/0026-reflection-everywhere.md),
[0035](../adr/0035-material-model.md), [0042](../adr/0042-space-toolbox-layering.md),
[luau-api guide](../guides/luau-api.md), [rhi-capabilities](../reference/rhi-capabilities.md),
[00-scope](00-scope.md), [BACKLOG](../backlog/BACKLOG.md).
