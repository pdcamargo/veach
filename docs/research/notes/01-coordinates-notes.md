# Q1 raw notes: large-world coordinates and reference frames

Research notes for [../01-coordinates.md](../01-coordinates.md). Every external claim has a link. Numbers marked
**(computed)** come from IEEE 754 arithmetic, worked out in this session with a short Python script (unit roundoff
2^-24 for float32 and 2^-53 for float64; ULP = 2^(floor(log2 x) - 23) and 2^(floor(log2 x) - 52)). Anything not
confirmed from a primary source is marked **(unverified)**.

## How much precision does each representation give at each place, at s = 1 and s = 0.01?

### Takeaway

Float32 runs out at about 16 km (1 mm ULP) and is useless beyond a planet's radius at s = 1. Float64 gives
nanometers inside a body's frame, 30 µm at 1 AU, 1 mm at 59 AU and 2 m at one light-year, so one double frame per
star system is enough; the galaxy (10^21 m, 65-131 km ULP) needs integers or a second level.

### Cited findings

- Single-precision floats represent every integer only between -16,777,216 and 16,777,216; at [32768; 65536] the
  step is ~0.0039 (float) vs ~1e-11 (double), the "maximum recommended single-precision range for any 3D game";
  Godot recommends about 2048 units for first-person, 4096 third-person, 16384 top-down — [Godot docs, Large world
  coordinates](https://docs.godotengine.org/en/stable/tutorials/physics/large_world_coordinates.html)
- Elite Dangerous needs "millimeter precision" for consistent visuals and gameplay; noise inputs are relative to the
  planet's center and reach tens of billions of millimeters; single floats would work only at "100s of km instead of
  10s of thousands of km" — [80.lv, Generating the Universe in Elite Dangerous](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)
- Jolt: "If all simulation takes place within roughly 5 km from the origin, floating point precision is accurate
  enough"; at 10,000 km from the origin "the resolution of the broad phase is reduced to 1 m" in double mode — [Jolt
  Physics docs, Big Worlds](https://jrouwe.github.io/JoltPhysics/index.html)
- Unreal 5's default WORLD_MAX is "88 million kilometers" (UE4 was 21 km) — [UE5 Large World
  Coordinates](https://dev.epicgames.com/documentation/en-us/unreal-engine/large-world-coordinates-in-unreal-engine-5)

Precision table **(computed)**, ULP in meters (worst rounding error is half of it):

| Place | Distance from frame origin | float32 ULP | float64 ULP |
|---|---|---|---|
| Base / cave, 1 km | 1e3 | 6.1e-5 | 1.1e-13 |
| 16 km | 1.6e4 | 9.8e-4 | 1.8e-12 |
| Earth radius at s = 0.01 | 6.37e4 | 3.9e-3 | 7.3e-12 |
| Earth radius at s = 1 | 6.37e6 | 0.5 | 9.3e-10 |
| Jupiter radius at s = 1 | 6.99e7 | 8 | 1.5e-8 |
| Moon orbit at s = 1 | 3.84e8 | 32 | 6.0e-8 |
| 1 AU at s = 0.01 | 1.50e9 | 128 | 2.4e-7 |
| 1 AU at s = 1 | 1.50e11 | 16,384 | 3.1e-5 |
| 30 AU (Neptune) at s = 1 | 4.49e12 | 5.2e5 | 9.8e-4 |
| 1 light-year | 9.46e15 | 1.1e9 | 2 |
| 100 ly | 9.46e17 | 6.9e10 | 128 |
| Galaxy radius 50,000 ly | 4.73e20 | 3.5e13 | 65,536 |
| Galaxy diameter 100,000 ly | 9.46e20 | 7.0e13 | 131,072 |

- float32 ULP reaches 1 mm at 16,384 m and 1 cm at 131,072 m **(computed)**.
- float64 ULP reaches 1 µm at 8.6e9 m (0.057 AU) and 1 mm at 8.8e12 m (59 AU, 0.00093 ly) **(computed)**.
- int64 fixed point: with a 1 mm LSB the range is ±9.2e15 m = ±0.97 ly; with 2^-16 m (15 µm) it is ±1.4e14 m =
  ±941 AU **(computed)**. 2^53 m (the largest integer a double holds exactly, and the JSON-safe integer limit) is
  0.95 ly or 60,209 AU **(computed)**.
- Time as double seconds: after 1 year the ULP is 3.7e-9 s, after 100 years 4.8e-7 s, after 10,000 years 6.1e-5 s;
  at Earth's heliocentric speed (29.78 km/s) that is 0.11 mm, 14 mm and 1.8 m of position error from time alone
  **(computed)**. This is why Q2's integer tick clock matters for positions.
- Rotation: a float32 quaternion has relative error ~2^-24, so a body's spin orientation stored in float32 misplaces a
  point on its surface by ~0.38 m at s = 1 and ~3.8 mm at s = 0.01 when converting into or out of the body-fixed
  frame; in float64, ~7e-10 m **(computed)**.

### Inferences

- At s = 0.01 a planet-centered float32 frame gives ~2 mm rounding at the surface: borderline but workable, and a
  float32 + floating-origin design would look fine there. At s = 1 the same design misplaces things by 0.25 m.
  A design that has to work at both scales with the same code can't be float32 in body frames.
- Double inside a star system is enough for everything up to the heliopause (~120 AU → ~2-4 mm ULP). Beyond about
  0.1 ly (0.25 m ULP) a system frame stops being good for close-range work; interstellar travel is FTL per 00-scope,
  so that's the natural boundary.
- The galaxy must not be one double frame for anything that needs better than ~100 km. It only needs that for
  directions and distances to other stars (fine), so integers per system are enough.

### Gaps

- No source found for SpaceEngine's internal representation (it is often said to use 64-bit floats and
  camera-relative rendering; **unverified**).

## What do shipped engines and games do?

### Takeaway

Everyone who ships large worlds ends up with the same three-part pattern: high-precision positions on the CPU
(doubles, or float plus an origin), local frames for physics (ship grids, body-local origins), and camera-relative
float32 on the GPU. They differ in where the boundary sits and whether the origin moves.

### Cited findings

- **Unreal 5 LWC.** Adds "double-precision data variant types" to core data types; Chaos physics "continues to
  function unabridged using the double type"; downcasts from `Chaos::FVec3` to `FVector3f` must be explicit;
  Niagara stays float and splits large worlds into grid cells (position within a cell plus a cell id) —
  [UE5 LWC](https://dev.epicgames.com/documentation/en-us/unreal-engine/large-world-coordinates-in-unreal-engine-5)
- **Unreal 5 LWC rendering.** The older FLWC types store `Tile * TileSize + Offset` with TileSize 256k; because the
  tile size is fixed, precision drops near tile edges, which before UE 5.4 showed as "oscillating precision as
  objects moved through the world, mostly visible as object jittering and vibration". DoubleFloat (DF) types replace
  it: two floats (High = value rounded to float, Low = most of the rounding error); "DFFast" variants trade precision
  for speed; many DF functions have a "substantial performance cost". Converting to translated (camera-relative)
  world space "as soon as possible" gives the best performance and precision; floats are safe there, and the page
  recommends refactoring toward it rather than promoting values to double. `PreViewTranslation` converts world to
  translated world — [UE5 LWC rendering](https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5?lang=en-US)
- Community reports that the render tile constant (`UE_LWC_RENDER_TILE_SIZE`, reported default 2097152.0) was
  lowered to fix jitter in first-person and driving projects (anecdotal; user modifications) —
  [Unreal forum thread](https://forums.unrealengine.com/t/lwc-large-world-coordinates-do-not-work-for-first-person-shooters-driving-games-wobbly-rendering-artefacts-ue5-only-ue4-works-fine/652748/19).
  Note the conflict with the docs' "256k" tile size: the forum value is probably a different constant or version
  **(unverified)**.
- **Unity HDRP camera-relative rendering.** Translates objects and lights by the negated camera position: it
  "replaces the world origin with the position of the Camera"; view and view-projection matrices, light and surface
  positions are camera-relative; "_WorldSpaceCameraPos is never Camera-relative"; on by default in ShaderConfig.cs —
  [Unity HDRP manual](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@14.0/manual/Camera-Relative-Rendering.html)
- **Godot double builds.** `precision=double` makes `Vector2/3/4` 64-bit; GPU shaders don't use doubles, the
  renderer emulates double precision with single floats; in double builds fragment-shader world coordinates can't be
  rebuilt from view space (compute in `vertex()` and pass as varyings); triplanar mapping and some particle modes
  don't gain precision; networked games must use the same build type on server and clients; origin shifting is the
  alternative for low-end platforms — [Godot docs](https://docs.godotengine.org/en/stable/tutorials/physics/large_world_coordinates.html)
- **KSP floating origin.** `FloatingOrigin` keeps an `offset`, has a default `threshold` of 500 (units not stated),
  shifts the origin to a reference position, re-registers world-space particle systems, and keeps a terrain shader
  offset aligned. **Krakensbane** is the velocity-frame system: it shifts the frame's velocity so the active vessel
  is near rest, tells FloatingOrigin whether a shift is safe this frame, and objects "not in the moving Krakensbane
  frame" get an extra offset — [KSP API, FloatingOrigin](https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html);
  the KSP wiki describes Krakensbane as changing "the reference point from Kerbin to the current vessel" —
  [KSP wiki, Kraken](https://wiki.kerbalspaceprogram.com/wiki/Kraken)
- KSP2 reportedly used "a local origin for planetary surface objects in addition to a floating origin like KSP1's"
  (from a search summary of a KSP2 developer post; the post was not fetched) **(unverified)** —
  [KSP dev diaries](https://kerbalspaceprogram.com/news/dev-diaries-developer-insights-10-collisions)
- **Star Citizen.** Chris Roberts called the 64-bit change "64-bit positional space" because star systems are
  millions of km across; Sean Tracy: "What we really needed was physics and positioning to be changed", not a whole
  engine conversion — [Gamers Nexus, Sean Tracy](https://gamersnexus.net/gg/2622-star-citizen-sean-tracy-64bit-engine-tech-edge-blending)
  (quotes via search summary). Zone system: objects grouped into "zone containers" rather than an octree (a planet a
  zone, a station another); ships and stations have their own local physics grid, so people move inside a ship
  regardless of where the ship is — [Gamers Nexus interviews](https://gamersnexus.net/gg/2119-chris-roberts-star-citizen-network-and-render-pipelines),
  [starcitizen.tools, Object Container Streaming](https://starcitizen.tools/Object_Container_Streaming) (fan wiki;
  2015-2017 era; current implementation may differ)
- **Elite Dangerous.** Noise libraries written in "64 bit – i.e. double precision and dual-float precision"
  ("emulated 64-bit functionality using two 32 bit floats") because "some GPUs handle one better than the other, or
  not at all"; positions relative to the planet's center; a 64-bit integer encodes sector x, y, z, the layer of an
  eight-layer octree, and the body id — [80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)
- **Outer Wilds.** Creative director Alex Beachum: fly away from the solar system and "everything's lost its mind
  ... because floating point errors build up" — [PC Gamer](https://www.pcgamer.com/how-to-build-a-black-hole/).
  That Outer Wilds uses a shifting floating origin comes only from fan discussion **(unverified)**.
- **Space Engineers.** Uses double-precision position types (VRageMath `Vector3D`) **(unverified; not fetched)**.
- Unity forum thread asking whether floating origin will be built into physics and netcode, i.e. it isn't —
  [Unity discussions](https://discussions.unity.com/t/floating-origin/902429) (title only, not fetched;
  **unverified** content).

### Inferences

- Unreal's history is the cautionary tale: fixed tiles gave jitter at tile edges and were replaced by double-float
  pairs; the docs' strongest advice is "get into camera-relative space as soon as possible and use floats". Veach
  can skip the tile stage entirely by doing the double → camera-relative subtraction on the CPU in extraction.
- Star Citizen's "local physics grids" and KSP2's "local origin for surface objects" are the same idea as a frame
  tree with ship and surface frames.
- A floating origin moves where rounding happens with the player's position; KSP needed Krakensbane, particle
  re-registration and terrain shader offsets to keep it consistent. In multiplayer with players in different places
  there is no single origin.

### Gaps

- No primary Star Citizen tech talk was fetched (CitizenCon videos). Quotes come from press interviews.
- Thatcher Ulrich's "Rendering Massive Terrains using Chunked LOD" (SIGGRAPH 2002) and "A Real-Time Planet" were not
  fetched; the standard idea attributed to that line of work, chunk-relative vertex positions with a per-chunk origin,
  is listed as **(unverified)** attribution.
- No source for Elite's exact CPU position type.

## What does it cost on Apple GPUs/CPUs and on Windows?

### Takeaway

Apple GPUs have no fp64 at all; anything double on the GPU is emulated (double-float pairs), with a cost the sources
call substantial but nobody quantifies for Apple silicon. CPU doubles are full-rate scalar, half-width SIMD. So the
design should keep the GPU float32 and camera-relative, and keep doubles on the CPU.

### Cited findings

- The Metal Shading Language has no double type (forum citing MSL spec section 2.3); Apple GPUs do not support FP64 —
  [Apple Developer Forums](https://developer.apple.com/forums/thread/709663);
  [MSL specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf). Apple's Myles
  Maxfield said Metal-based WebGPU implementations would not offer an fp64 extension —
  [W3C public-gpu list](https://lists.w3.org/Archives/Public/public-gpu/2022Apr/0006.html).
- A developer working on FP64 emulation on Apple GPUs found "the double-single approach with float32 is not precise
  enough" for scientific use and built a non-IEEE "eFP64"; no throughput figures —
  [philipturner gist](https://gist.github.com/philipturner/0d47f5e925bb3a9568d3c4d6dca19a1b)
- Unreal: "Math with LWC takes longer to compute"; DF functions carry a "substantial performance cost"; materials
  still use tile-offset for performance — [UE5 LWC rendering](https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5?lang=en-US)
- Jolt: naive all-double is "more than 2x slower" than float; Jolt's double mode (doubles only for positions, broad
  phase and rotations in float) is 5-10% slower than float mode — [Jolt docs](https://jrouwe.github.io/JoltPhysics/index.html)
- Godot: double builds carry performance and memory penalties; mid- to high-end desktops; origin shifting for
  low-end — [Godot docs](https://docs.godotengine.org/en/stable/tutorials/physics/large_world_coordinates.html)

Computed for Veach **(computed)**:

- Camera-relative float32 is always sub-pixel: a pixel at 1080p with a 60° vertical field of view covers
  1.07e-3 × distance; float32's relative ULP is 1.19e-7, i.e. 1.1e-4 of a pixel at any distance.
- Per-frame upload of camera-relative instance matrices: 100k instances × 48 B = 4.8 MB/frame, 288 MB/s at 60 fps.
- Velocities in float32 are fine: ULP 4.9e-4 m/s at 7.7 km/s (low orbit, s = 1), 2e-3 m/s at 29.78 km/s.

### Inferences

- On ARM64 (M1, M3 Pro) NEON is 128-bit: 2 doubles vs 4 floats per vector op; scalar double has about the same
  latency as float. Transform propagation and extraction in double should cost little more than in float at the
  entity counts the lab has; measure in a spike, don't assume.
- M1-class vs M3 Pro-class GPU: no difference in kind (neither has fp64). The difference only shows where emulated
  double-float runs on the GPU (body-relative procedural noise, Q4/Q5/Q8); keeping that out of per-pixel hot paths
  matters more on M1.
- Windows/MSVC x64: doubles are SSE2, no x87 issues on x64. FMA contraction defaults differ between Clang ARM64 and
  MSVC x64 (Q12) **(unverified defaults; Jolt advises `-ffp-contract=off`)**.

### Gaps

- No measured double-float throughput on any Apple GPU. A spike must measure it if a GPU path needs it.
- MSVC's default floating-point contraction behaviour under `/fp:precise` not verified from Microsoft docs.

## Frames, rotating bodies, handoff and fictitious forces

### Takeaway

Rotating frames are cheap to get right in double and expensive in float32 (0.38 m at s = 1 from a float spin
quaternion). Handoff between frames must convert velocity (ω × r is 465 m/s at Earth's equator at s = 1).
Fictitious forces are negligible at Earth-like day lengths and become gameplay-visible with short days on small
planets.

### Cited findings

- KSP Krakensbane moves the velocity frame with the active vessel, separately from the position origin —
  [KSP API](https://kspmoddinglibs.github.io/KSPDocsSite/class_floating_origin.html)
- Star Citizen ships and stations each have a local physics grid — [Gamers Nexus](https://gamersnexus.net/gg/2119-chris-roberts-star-citizen-network-and-render-pipelines)
- Jolt collision queries take a base offset; results are floats relative to it; choose it "close to where collision
  results are expected"; "it is not possible to rotate a RVec3 by a Quat" — [Jolt docs](https://jrouwe.github.io/JoltPhysics/index.html)

Computed **(computed)**, for Earth's sidereal day (86,164 s, ω = 7.29e-5 rad/s) and a 30-minute day:

| Day | s | Surface speed ω R | Centrifugal ω² R | Coriolis at 100 m/s (2 ω v) |
|---|---|---|---|---|
| Earth | 1 | 465 m/s (7.7 m per tick) | 0.034 m/s² (0.35 % g) | 0.015 m/s² |
| Earth | 0.01 | 4.6 m/s | 3.4e-4 m/s² | 0.015 m/s² |
| 30 min | 1 | 22.2 km/s (beyond breakup: centrifugal 7.9 g) | 77.6 m/s² | 0.70 m/s² |
| 30 min | 0.01 | 222 m/s | 0.78 m/s² (7.9 % g) | 0.70 m/s² (7 % g) |

- Circular low-orbit speed with surface g held at 9.81: 7.67 km/s (128 m per tick) at s = 1, 767 m/s (12.8 m per
  tick) at s = 0.01 **(computed)**.

### Inferences

- Ignoring Coriolis and centrifugal terms (00-scope's default) is safe for Earth-like days at any s, and a visible
  simplification for NMS-style short days on small planets. Make it a per-body option in Q9/Q10, off by default.
- Q2/Q3 must clamp day length above the breakup spin (ω² R < g), at least for physically generated bodies.

### Gaps

- KSP's altitude threshold for switching between inertial and rotating frames was not found in a primary source
  **(unverified)**.

## Integers on wires: JSON, MCP, Luau, record/replay (Veach code)

### Takeaway

Today's code silently loses int64 above 2^53 in Luau and on any JavaScript JSON client, and has float32 everywhere a
position goes. These are local fixes, but the types must change before any toolbox code exists.

### Cited findings (repository)

- `engine/script/src/marshal.cpp` loads `EcsI64`/`EcsU64` through `double` (`load_number` returns double;
  `store_number` casts back): any 64-bit field above 2^53 is rounded in Luau. Entities are already tagged userdata
  (`entity_tag`), so entity ids are safe.
- `marshal.cpp` maps `ValueKind::vec3` to Luau `vector` via `lua_pushvector` / `lua_tovector` (float32), per
  [ADR 0013](../../adr/0013-conventions-and-math.md).
- `engine/ecs/include/veach/ecs/transform.hpp`: `Transform { vec3 translation; quat rotation; vec3 scale; }` and
  `GlobalTransform { mat4 matrix; }`, all float32; propagation runs per fixed step and again before extraction.
- `engine/render/src/renderer.cpp` "views" extractor builds `view.view = inverse(world_from_camera)` from
  `GlobalTransform` in float32 world space; "scene" extractor copies `global.matrix` into `MeshDraw.world`; lights
  take `g.translation()`.
- `engine/render/src/atmosphere.cpp:220`: camera altitude is `max(view.position.y, 0) * 0.001` km: altitude is world
  Y, zenith is world +Y.
- `engine/render/src/scene_renderer.cpp`: the photo-mode TLAS takes instance transforms from the draws (world space).
- Record/replay hashes every Scriptable component serialized through reflection (FNV-1a) — [record-replay
  spec](../../specs/record-replay/spec.md).

### Inferences

- JSON: doubles survive JSON and JavaScript exactly if written with round-trip precision (nlohmann::json writes
  shortest round-trip form, **unverified** for our version); int64 must be strings.
- The replay hash over doubles is deterministic only if the doubles themselves are (Q12).

### Gaps

- None blocking; these are code facts.
