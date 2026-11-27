# Q5 raw notes: the procgen graph model

Raw research notes for [05-procgen-graph.md](../05-procgen-graph.md). Gathered 2026-11-27 by web research (three
parallel research passes plus direct searches) and by reading the repository. Every external claim has a link;
anything not confirmed against a primary source is marked **UNVERIFIED**. Repository facts cite the file.

## Graph tools: data model, execution, extension

### Takeaway
Every mature tool separates the graph (data) from its execution, evaluates in batches over many elements, offers two
tiers of user nodes (native API plus a script or shader escape hatch), and treats global operators (erosion) as the
thing that breaks tiling, resolution independence and determinism.

### Cited Findings
- Unreal PCG custom node = a `UPCGSettings` subclass (parameters, pins) plus an `IPCGElement` whose
  `ExecuteInternal(FPCGContext*)` is called until it returns true, so a node can time-slice itself —
  [UE API: IPCGElement](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/PCG/IPCGElement?lang=en-US);
  [Blueshift tutorial](https://blueshift-interactive.com/2025/09/03/how-to-create-custom-pcg-nodes/)
- Unreal PCG Blueprint nodes run through `FPCGExecuteBlueprintElement` — [ag2s.net](https://ag2s.net/unreal-pcg1-introduction-and-basic-pcg-objects/)
- Unreal PCG Hierarchical Generation: a graph has a default grid size, Grid Size nodes override it per branch; data
  cascades from larger grids to smaller ones, never back; an "Unbounded" grid executes once; runtime generation uses
  one generation radius per grid size and cleans up at radius × multiplier —
  [UE docs: generation modes](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-generation-modes-in-unreal-engine);
  example 3200 grid / 4800 cm radius, 6400 grid / 9600 cm —
  [UE biome core reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-pcg-biome-core-and-sample-plugins-reference-guide-in-unreal-engine)
- Unreal PCG GPU execution (Beta): connected GPU nodes are grouped into one "compute graph"; CPU↔GPU transfers are
  shown as badges on nodes and the guidance is to group GPU nodes to minimize them; "Skip Readback to CPU" when only the
  GPU consumes the result; Custom HLSL node "source code is injected into a compute shader and executed over data
  elements in parallel" (kernel types point processor, point generator, attribute processor, custom); results depend on
  point seeds, `ComputeSeedFromPosition()` recommended; **GPU-spawned instances live only in GPU memory: no collision,
  navigation, baked lighting or HLOD** — [UE docs: PCG with GPU processing](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-with-gpu-processing-in-unreal-engine)
- Unreal PCG graph storage is binary `.uasset` — UNVERIFIED (could not open primary page). No determinism claim
  across hardware in the docs — UNVERIFIED.
- Blender Geometry Nodes: node groups are converted to a "lazy-function graph" and evaluated —
  [commit 4130f1e](https://developer.blender.org/rB4130f1e); only needed inputs/outputs are computed (a Switch evaluates
  only the active branch) — [3.0 release notes](https://developer.blender.org/docs/release_notes/3.0/nodes_physics/);
  fields are evaluated in batches by "multi-functions" optimized for many elements —
  [T98940](https://developer.blender.org/T98940), [T90875](https://developer.blender.org/T90875); evaluator goals:
  multithreading, laziness, non-recursive evaluation, merging small math groups into one multi-function —
  [T98492](https://developer.blender.org/T98492)
- Houdini VEX: "loosely based on C", VOP networks compile to VEX, performance "close to compiled C/C++", VEX SOP "10
  or more times faster than a point SOP" — [SideFX VEX docs](https://www.sidefx.com/docs/houdini/vex/index.html); VEX
  runs per point, threaded — [VEX SOP](https://www.sidefx.com/docs/houdini/nodes/sop/vex.html); VEX VM is SIMD over
  batches of ~256 points (forum, old versions) — [forum 23745](https://www.sidefx.com/forum/topic/23745/)
- Houdini PDG/TOPs: work items with attributes; HDA Processor cooks a digital asset per work item —
  [TOPs intro](https://www.sidefx.com/docs/houdini/tops/intro.html); Houdini Engine for Unreal imports PDG outputs
  asynchronously as temporary assets until baked — [Houdini Engine PDG](https://www.sidefx.com/docs/houdini/unreal/pdg.html)
- Gaea: "due to the nature of these algorithms and parallel processing the erosion flow may differ slightly each
  time" — [Gaea erosion](https://docs.quadspinner.com/Reference/Erosion/Erosion.html); normal builds max 8192²,
  tiled builds split into buckets — [build types](https://docs.quadspinner.com/Guide/Build/Build-Types.html); nodes
  needing data outside their tile seam — [tiled builds](https://docs.gaea.app/using/using-gaea/build-and-export/tiled-builds.html);
  bake non-tiling stages (erosion) at full-world resolution —
  [resolution without seams](https://docs.gaea.app/guides/scenarios/large-projects/resolution-tiles-without-seams.html)
- World Machine: erosion ignores neighbouring tiles' heights, blended afterwards —
  [forum](https://forum.world-machine.com/t/single-vs-tiled-significant-render-differences/4408); erosion "can change
  behavior completely depending on scale", workarounds "Scale independence" option and per-node resolution —
  [forum](https://forum.world-machine.com/t/how-to-mitigate-resolution-dependencies/6424)
- Unity Shader Graph shows "View Generated Shader" (graph compiles to HLSL) —
  [Cyanilux (third party)](https://www.cyanilux.com/tutorials/intro-to-shader-graph/); `.shadergraph` is JSON —
  UNVERIFIED. VFX Graph simulates on GPU compute — [Unity glossary](https://unity.com/glossary/vfx-graph). Unity
  Graph Toolkit (experimental, Unity 6.2) is a framework for custom graph tools —
  [Unity discussions](https://discussions.unity.com/t/unity-s-graph-toolkit-experimental-available-today-in-unity-6-2/1664909)
- FastNoise2: a node tree of generators, fractals, domain warp, modifiers; operators run inside the SIMD pipeline so
  intermediates stay in registers; 2D/3D/4D; thread-safe evaluation of one tree from many threads; FastSIMD compiles
  Scalar/SSE2/SSE4.1/AVX2/AVX512/NEON/WASM and picks at runtime; README warns MSVC has SIMD bugs producing wrong
  output, recommends ClangCL; node trees encode to a compact string; a Node Editor with 2D/3D previews can push live
  tree updates to an application; MIT; author's numbers (i7-7820X, AVX2, million points/s): 3D Perlin 261 vs 48
  (FastNoise Lite), 3D Simplex 268 vs 37, 3D Cellular 52 vs 12, 2D Perlin 624 vs 93 —
  [FastNoise2 README](https://github.com/Auburn/FastNoise2)

### Inferences
- A per-sample "field graph" interpreted in SIMD batches (FastNoise2, Blender multi-functions, Houdini VEX) gets
  near-native speed without JIT, because interpretive overhead is paid per node per batch, not per sample.
- Unreal's GPU instances have no collision: the same split Veach needs ("GPU output is visual-only").
- Unreal's hierarchical grids with a one-way cascade (large → small) map directly onto "coarse cached stages feed
  chunk-local ones" (00-scope finding 2).
- Global operators (erosion, drainage) must be coarse whole-body stages at a fixed resolution, not field nodes;
  Gaea/World Machine show what happens otherwise (seams, resolution dependence, non-determinism).
- FastNoise2's MSVC warning matters: Windows is CI-only for Veach; a SIMD evaluator must be tested there.

### Gaps
- Exact storage formats of UE PCG, Houdini HDA, Gaea, World Machine graphs (not fetched).
- FastNoise2 seeding semantics and cross-ISA bit equality (README silent).

## Shipped games: CPU/GPU split, authoring, gameplay use

### Takeaway
Shipped games author placement and terrain rules once and run them on the GPU for visuals, but give gameplay its own
path (CPU evaluation or a batched readback treated as non-authoritative), and keep expensive non-local work offline or
in coarse stages.

### Cited Findings
- Horizon Zero Dawn (van Muijden, GDC 2017): moved from offline CPU bake → GPU bake → real-time GPU placement; goals
  "Quick iterations, Large variety, Believable look, Art Directable, Data driven, Deterministic, Locally stable"; a node
  graph (likened to Nuke/Substance) where each network outputs one 2D density map per asset/group from "WorldData"
  maps (roads, water, height); "ecotopes" bundle assets, distribution, colour, weather, effects, sound, wildlife;
  footprints set spacing (trees ~6 m, undergrowth ~1 m); WorldData ~4 MB/km² at 0.5-2 m resolution; graph **compiled
  to an intermediate form**, thousands of layers merged into hundreds; 64×64 density map per block (128 m trees, 32 m
  grass); generate by ordered dithering (~10 µs), placement (~7 µs), RNG seeded from pattern index and ID; **an
  in-game GPU interpreter runs the same intermediate form for debugging**; deterministic except emit order (stable ids
  carried); results packed and copied to CPU in one transfer; 500+ asset types, 100,000+ objects, "~250µs avg busy load
  on GPU" (unit/platform UNVERIFIED) —
  [slides transcript](https://www.slideshare.net/slideshow/gpubased-procedural-placement-in-horizon-zero-dawn/72897079),
  [GDC Vault](https://www.gdcvault.com/play/1024120/GPU-Based-Run-Time-Procedural),
  [PDF](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf)
- Far Cry 5 (Carrier, GDC 2018): "100 square km of wilderness with a terrain changing every day"; tools for biomes,
  terrain texturing, freshwater networks, cliffs, power lines, fog density map, world map —
  [GDC Vault](https://gdcvault.com/play/1025557/Procedural-World-Generation-of-Far),
  [80.lv](https://80.lv/articles/houdini-procedural-world-generation-of-far-cry-5/),
  [PlayStation blog](https://blog.playstation.com/2018/03/22/the-procedural-world-generation-of-far-cry-5/).
  Houdini Engine in the Dunia editor, regenerated on terrain change, baked output, no runtime procgen — UNVERIFIED.
- No Man's Sky (McKendrick, GDC 2017): voxel generation → polygonization → texturing → population → simulation,
  "continuously in real-time" —
  [gamedeveloper.com](https://www.gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-),
  [GDC Vault](https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No). "Uber noise" is fractal noise with
  erosion-like terms, co-developed by Giliam de Carpentier — [modding project (secondary)](https://github.com/gistya/nomansterrain).
  CPU job threads for voxels and meshing, data-file metadata for biomes — UNVERIFIED.
- Elite Dangerous: per-planet properties "packed up into a buffer which is sent to the GPU"; patches spherified by
  "compute shaders on your GPU"; noise from patch position, planet ID and astronomical data; cube-based quadtree;
  native-double and emulated two-float noise versions for GPUs that handle one better; **uniform physics patches
  generated separately: "there is always a physics patch available for any player or non-player entity that requires
  it"**; 64-bit hierarchical ids so "each client and the server knows you're talking about the exact same object" —
  [80.lv Doc Ross interview](https://80.lv/articles/generating-the-universe-in-elite-dangerous/). Whether physics
  patches come from CPU evaluation or readback: UNVERIFIED.
- KSP2 "Planet Tech" (Developer Insights #12): planet vertex data stays on GPU, colliders built separately around
  colliders, not the camera — search snippet only, page 404 — UNVERIFIED.
- Hobbyist reports of CPU/GPU noise mismatch breaking collision and hashes differing between compiles —
  [Godot forum](https://forum.godotengine.org/t/help-with-procedural-planets/139951),
  [three.js forum](https://discourse.threejs.org/t/space-person-multiple-planets-clouds-biomes-flying-and-walking/90091.md)
  (anecdotal, not studio sources).
- Media Molecule's Dreams: SIGGRAPH 2015 "Learning from Failure" on SDF-based renderers for user-authored content —
  [slides](https://advances.realtimerendering.com/s2015/AlexEvans_SIGGRAPH-2015-sml.pdf),
  [blog](https://www.mediamolecule.com/blog/article/siggraph_2015). Detail of GPU evaluation of edit lists —
  UNVERIFIED (slides not read).

### Inferences
- HZD is the strongest precedent for Veach: one graph → compiled intermediate form → executed on GPU, with an
  interpreter over the same form for debugging. Veach can invert it: the interpreter is the product, compiled
  specialization is the optimization.
- Elite's "physics patch for any entity that needs it" is the CPU-authoritative path: generate collision near
  entities (not near the camera) from the same function definition.
- "Density, then discretize" (HZD) lets gameplay query densities on the CPU without the GPU.

### Gaps
- Far Cry 5 and Ghost Recon Wildlands specifics (videos only).
- How NMS designers author rules (metadata vs graphs).

## Execution technology: Luau, Slang, deterministic math

### Takeaway
Luau is a fast interpreter with an opt-in native JIT, but its trig calls the platform libm and its `math.noise` is
float32 with a 256 period, so per-sample noise and authoritative math must not come from Luau's builtins. Slang's
precompiled modules plus link-time types are a ready mechanism for "a user supplies one function behind an
interface". Factorio is the precedent for "script declares the expression graph, native code compiles and runs it".

### Cited Findings
- Luau interpreter is "noticeably faster than Lua 5.x" and on some workloads "can match the performance of LuaJIT
  interpreter" — [luau.org/performance](https://luau.org/performance)
- Luau.CodeGen: optional JIT for x64 and arm64, compiles functions/modules the host marks; no automatic tracing
  decisions; type annotations specialize code paths — [luau.org/performance](https://luau.org/performance); out of
  preview July 2024 with full x64 and A64 support, falls back to the interpreter on unsupported hardware —
  [recap July 2024](https://luau.org/2024/07/23/luau-recap-july-2024.html),
  [recap Oct 2023](https://luau.org/2023/11/01/luau-recap-october-2023.html); enabled with `--!native` / `@native`,
  costs memory and startup, hard limits (64K instructions per block, 1M per script), best for "heavy numeric
  computation on tables and buffers" — [Roblox docs](https://create.roblox.com/docs/luau/native-code-gen)
- No official codegen speedup figure; anecdotal ~2× on a prime benchmark —
  [devforum](https://devforum.roblox.com/t/luau-native-code-generation-preview-studio-beta/2572587) (anecdotal)
- `vector` is a native float32 ×3 value type with VM fast paths, immutable; `buffer` is a fixed-size mutable byte
  block up to 1 GiB with f32/f64/int read/write, bounds-checked — [luau.org/library](https://luau.org/library)
- Fastcall builtins cover all of `math` except `noise` and `random` — [luau.org/performance](https://luau.org/performance)
- `math.sin` is `lua_pushnumber(L, sin(luaL_checknumber(L, 1)))`: platform libm, double —
  [lmathlib.cpp](https://raw.githubusercontent.com/luau-lang/luau/master/VM/src/lmathlib.cpp). Whether the codegen and
  fastcall paths also call libm: UNVERIFIED (likely).
- `math.noise` is 3D Perlin on float inputs, range [-1, 1], with `fmod(x, 256.0)` wrapping behind a flag —
  [lmathlib.cpp](https://raw.githubusercontent.com/luau-lang/luau/master/VM/src/lmathlib.cpp),
  [library](https://luau.org/library)
- `lua_Callbacks::interrupt` runs at safepoints (loop back-edges, calls, GC); the host can stop runaway code there —
  [luau.org/sandbox](https://luau.org/sandbox); no built-in timeout API, `interrupt` is the tool —
  [discussion #239](https://github.com/luau-lang/luau/discussions/239). Setting it from another thread is safe per
  source comments — via [docs.rs bindings](https://docs.rs/luau-src/0.3.0/luau_src/vm/struct.lua_Callbacks.html)
  (re-check against the pinned `lua.h`).
- Slang link-time specialization: precompile to `.slang-module` (binary IR), then link with `extern static const` and
  `extern struct S : IFoo` resolved by another module's `export`; specialization reuses precompiled work;
  `isBinaryModuleUpToDate` checks staleness — [Slang user guide: link-time specialization](https://shader-slang.org/slang/user-guide/link-time-specialization.html)
- Slang runtime API: `IGlobalSession` creation "can take a significant amount of time" and is not thread-safe;
  `ISession` per target set; `loadModule` / `loadModuleFromIRBlob` → composite → `link()` → `getTargetCode` —
  [Slang user guide: compiling](https://shader-slang.org/slang/user-guide/compiling.html). No compile-time figures.
- Slang has a `FloatingPointMode` compiler option — [compiling](https://shader-slang.org/slang/user-guide/compiling.html);
  the exact `slangc -fp-mode` spelling: UNVERIFIED.
- Metal `MTLCompileOptions.fastMathEnabled`: "The default value is true" (deprecated in favour of `mathMode`) —
  [Apple docs](https://developer.apple.com/tutorials/data/documentation/metal/mtlcompileoptions/fastmathenabled.json).
  For the offline `xcrun metal` path Veach uses, the equivalent default (fast math on unless `-fno-fast-math`):
  UNVERIFIED.
- Jarzynski & Olano, "Hash Functions for GPU Rendering", JCGT 2020: Pareto study of integer hashes, recommends PCG
  variants (including `pcg3d`) — [JCGT paper](https://www.jcgt.org/published/0009/03/02/paper.pdf),
  [Reed's summary](https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering)
- Float-based shader hashes diverge between CPU and GPU (24.323999 vs 24.324001 amplified by `fract()`); integer
  hashes fed from float seeds also diverge; identical noise needs integer lattice coordinates throughout —
  [danilw blog](https://danilw.github.io/blog/Hash_Noise_in_GPU_Shaders)
- Bit-exact CPU/GPU float equality is "hard to do across hardware architectures"; mitigations are explicit operation
  order or fixed point — [Intel forum](https://community.intel.com/t5/OpenCL/GPU-and-CPU-floating-point-equivalence/m-p/1084936)
- Factorio noise expressions (FFF #390): Lua data (math-like strings) → C++ tokenizer, shunting-yard, AST → compiled
  "noise program" per surface, run per chunk, multithreaded and SIMD; three programs per surface, each "one procedure
  with multiple outputs" (up to 20% faster generation, up to 50% faster compile); constant folding deferred until map
  settings are known; hash-based global dedup (31,000 → 5,300 objects, 125 MB saved); Vulcanus chunk generation
  18.35 ms → 2.83 ms; a noise visualizer was "invaluable" — [FFF #390](https://factorio.com/blog/post/fff-390);
  expression trees since [FFF #258](https://factorio.com/blog/post/fff-258); executed for every tile —
  [Factorio API docs](https://lua-api.factorio.com/latest/auxiliary/noise-expressions.html)

### Inferences
- Luau's role is to **declare** graphs, rules and per-record logic, never per-sample loops: the interpreter runs
  per-sample code orders of magnitude slower than SIMD C++ (Luau throughput not measured here; see spike S2), and its
  libm calls break cross-platform determinism.
- A procgen Luau context can replace `math` with a deterministic implementation (same names) and remove
  `math.random`, `math.noise` and `os.clock`, so hook authors get determinism without learning a new API.
- Integer lattice hashing (PCG family) is bit-exact everywhere; gradient and interpolation floats are not
  guaranteed bit-exact CPU vs GPU, so "GPU is visual-only" stays the rule even with the same graph.
- Slang link-time types let a C++ module ship a precompiled node implementation that the graph's generated Slang
  links against at run time once VCH-009 exists; until then, a GPU interpreter over a fixed node set needs no runtime
  compilation.

### Gaps
- Measured Luau interpreter and codegen throughput on Apple silicon for numeric loops (spike).
- Slang runtime compile latency for a generated module (no figures found).
- Whether Luau's VM C code can be FMA-contracted by Clang/MSVC between operations (unlikely across bytecodes,
  UNVERIFIED).

## Hardware: M1-class vs M3 Pro-class

### Cited Findings
- M3 Pro: ~150 GB/s unified memory bandwidth (192-bit LPDDR5-6400), down from 200 GB/s on M1 Pro/M2 Pro —
  [MacRumors](https://www.macrumors.com/2023/10/31/apple-m3-pro-less-memory-bandwidth/),
  [AnandTech](https://anandtech.com/show/21116); Apple claims M3 GPU matches M1 at nearly half the power and up to
  65% more peak performance — [Apple newsroom](https://www.apple.com/newsroom/2023/10/apple-unveils-m3-m3-pro-and-m3-max-the-most-advanced-chips-for-a-personal-computer/)
- Base M1: 8-core GPU, ~2.6 TFLOPS FP32, 68 GB/s, 4 performance + 4 efficiency CPU cores — UNVERIFIED in this pass
  (widely quoted; not fetched).
- M3 Pro: up to 18-core GPU, up to 12-core CPU (6P + 6E) — UNVERIFIED (from the Apple newsroom headline, not read in
  full).

### Inferences
- Bandwidth ratio M1 → M3 Pro is ~2.2× (68 → 150 GB/s); GPU compute ratio roughly 2-3× (UNVERIFIED core counts and
  clocks). Field evaluation is ALU-bound (noise), scatter and meshing are more bandwidth-bound.

## Repository facts (Veach as it is)

- Luau: strict, sandboxed (`luaL_sandbox`, `luaL_sandboxthread`), one module `@veach`, command-buffer transactions,
  batched system calls; **no yielding, no `require` of user modules, no interrupt or timeout yet**; script component
  fields limited (no nested tables, arrays) — [ADR 0007](../../adr/0007-scripting-luau.md),
  [luau-api guide](../../guides/luau-api.md)
- Luau native codegen and budgets are backlog VCH-034 — [BACKLOG](../../backlog/BACKLOG.md)
- Slang compiled offline only; `VEACH_SHADER_FLAGS` = `-matrix-layout-column-major -warnings-as-errors all
  -warnings-disable 41017`, **no floating-point mode flag**; Metal path is `xcrun metal -std=metal3.1` with no
  `-fno-fast-math` — `cmake/VeachShaders.cmake`, [ADR 0009](../../adr/0009-shader-compilation.md). Runtime compilation
  is VCH-009.
- Materials: Slang interfaces with default implementations, `override` one function, `standard*` defaults public
  so overrides extend them, kinds registered at build time into a generated static switch —
  [ADR 0035](../../adr/0035-material-model.md). ADR 0008 already anticipates "a node graph (a future nature or material
  editor) can generate Slang".
- Reflection is mandatory and one walker feeds JSON, JSON Schema, Luau types, MCP; `unsupported` kinds (arrays, maps)
  are skipped by JSON and Luau today — [ADR 0026](../../adr/0026-reflection-everywhere.md)
- ADR 0042 names "a procgen graph runtime" explicitly as a generic need that moves down into `engine/` —
  [ADR 0042](../../adr/0042-space-toolbox-layering.md)
- `veach::Rng` is PCG32, public plain state, `next_float` from 24 bits — `engine/core/include/veach/core/rng.hpp`
- Single-threaded today; job system is VCH-015 (enkiTS or Taskflow, undecided) — [ARCHITECTURE](../../ARCHITECTURE.md#threading-model)
- RHI gaps relevant here: indirect dispatch/draw (VCH-071), 3D textures (VCH-072), non-blocking readback (VCH-073),
  async compute (VCH-074), texture updates after creation (VCH-076). Null backend: every call succeeds and does
  nothing — [rhi-capabilities](../../reference/rhi-capabilities.md)
- Hot reload: windowed polls every 250 ms, headless reloads on MCP `reload`; failed reload keeps the old version —
  [luau-api guide](../../guides/luau-api.md#hot-reload)
