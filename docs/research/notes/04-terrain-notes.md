# Q4 terrain: raw notes

Raw research notes behind [04-terrain.md](../04-terrain.md). Collected 2026-11-27. Every claim carries a link. Claims
from memory of standard references that I did not fetch are marked **(unverified)**. Numbers marked **(derived)** are
my own back-of-envelope arithmetic, shown so the spikes can check them.

Repository inputs read: `notes/_brief.md`, `00-scope.md`, `reference/rhi-capabilities.md`, backlog VCH-015 and
VCH-071..076, ADRs 0003, 0011, 0029, 0030, 0040, 0042, `ARCHITECTURE.md`, `specs/mcp-server/spec.md`.

## 1. Representation: heightfield, density volume, SVO or brick map; caves; no-surface bodies; asteroids; water

### Takeaway
Every shipped game with caves and digging everywhere (No Man's Sky, Space Engineers, Astroneer, Planet Nomads,
Roblox, Enshrouded) stores terrain as a volumetric scalar field generated from a function and keeps only player
changes as data. The heightfield-only planet engines (CDLOD, Outerra, Elite, KSP, Star Citizen) give cheap far LOD
but no caves or overhangs, so for Veach a heightfield is a generator ingredient and an acceleration, not the
representation.

### Cited Findings
- No Man's Sky's GDC 2017 talk covers its pipeline "from voxel-based world generation, through polygonization" —
  [Game Developer write-up](https://gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-);
  [GDC Vault](https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No).
- A thesis lists NMS as a game "that utilizes scalar field terrain with dual contouring" —
  [Blekinge thesis](https://www.diva-portal.org/smash/get/diva2:1333567/FULLTEXT02) (secondary).
- Zylann's summary of the NMS GDC talk: "They use a custom dual contouring with LOD", seams handled "by simply
  polygonizing a small distance over neighboring blocks", "using a lower isolevel so they match up" —
  [godot_voxel issue #24](https://github.com/Zylann/godot_voxel/issues/24) (secondhand summary of
  [the talk video](https://www.youtube.com/watch?v=sCRzxEEcO2Y)).
- NMS Worlds Part I (5.00, July 2024): "Terrain generation has been rewritten to incorporate dual marching cubes voxel
  meshing, which reduces vertex count, increases terrain generation speed, improves framerate and saves significant
  amounts of memory" — [nomanssky.com Worlds Part I](https://www.nomanssky.com/worlds-part-I-update/) (quoted via
  search results and the issue above; page itself not fetched).
- Space Engineers: procedural asteroids "are stored as a noise function", voxels are not kept in memory after
  generation, "only player changes to shape or material are made permanent"; the voxel LOD system went from two levels
  (with all voxel data in memory) to "many levels of detail"; world objects moved to 64-bit doubles while Havok stayed
  32-bit; world split into clusters of 20 km min, 50-100 km common —
  [Marek Rosa, Dec 2014](https://blog.marekrosa.org/2014/12/space-engineers-super-large-worlds_17/).
- Space Engineers planets: "up to 120 km in diameter", moons up to 19 km, "fully destructible, volumetric and
  persistent"; mountain ranges "goes flat once you hit a critical distance"; blocks on planets "can't be
  axis-aligned" — [Marek Rosa, Nov 2015](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/).
- Space Engineers planet data is six per-face heightmaps (front.png ... up/down) plus per-face material maps
  (`up_mat.png`); 2048 px is common, 4096 also used; "Heightmap resolution must be divisible by 8, and after that a
  power of 4" — [Procedural Ore Generator](https://github.com/asrbic/Procedural_Ore_Generator),
  [Keen support: height maps](https://support.keenswh.com/spaceengineers/pc/topic/28675-please-consider-fixing-the-planet-height-maps),
  [Keen support: misaligned lakes](https://support.keenswh.com/spaceengineers/pc/topic/26835-misaligned-lakes-on-earthlike-and-alien)
  (community sources). So SE is a hybrid: a cube-face heightfield drives a volumetric field; edits are voxels.
- SE engine namespace has `Planet` and `Storage` packages with `IMyOctreeLeafNode` and `IMyIsoMesher` interfaces —
  [SE API docs](https://fresc81.github.io/SpaceEngineers/namespace_sandbox_1_1_engine_1_1_voxels.html) (suggests an
  octree store and a mesher behind an interface; internals not documented).
- Planet Nomads: "procedural, destructible planets of 120 km in diameter", octree chunks "from 16 m to 128 km",
  1 m minimum voxel, chunks of "16x16x16 voxels", a "modified Marching Cubes algorithm", "3D noise algorithms are
  interpolated on a sphere", 2 km ground visibility with vehicles up to 120 km/h, generation in C++ threads,
  modifications in an R*-tree in SQLite —
  [Game Developer: Planet Nomads](https://gamedeveloper.com/programming/bending-unity-to-carry-spherical-voxel-planets-in-planet-nomads).
- Astroneer: "procedurally generated in a uniquely rendered voxel engine", deformable "as though it were made of
  Play-Doh" — [IGF page](https://igf.com/astroneer); caves to the core in 1.0 —
  [UE spotlight](https://www.unrealengine.com/en-US/spotlights/how-system-era-softworks-leveraged-ue4-to-create-astroneer-s-wonderful-universe).
  Marching cubes is a forum guess only ([UE forum](https://forums.unrealengine.com/t/how-to-make-this-voxels/77249)), **unverified**.
- Enshrouded: Keen Games' Holistic Engine is voxel-based with freely modifiable terrain; no technical internals
  published — [PCGH tech test](https://www.pcgameshardware.de/Enshrouded-Spiel-74267/Specials/Test-Release-Steam-Gameplay-1438724/)
  (press).
- Deep Rock Galactic: UE4, fully destructible procedural caves; voxel size and mesher are community guesses only
  (~0.5 m voxels) — [UE spotlight](https://www.unrealengine.com/spotlights/how-ghost-ship-games-found-success-with-deep-rock-galactic?lang=en-US);
  details **unverified**.
- Roblox smooth terrain: each voxel is a material (one of 23) plus 8-bit occupancy, 2 bytes; 32³ storage chunks
  (64 KB raw) with 16³, 8³ and 4³ mips used for rendering LOD; RLE on disk and wire; one level with ~1 billion
  non-empty voxels is 38 MB on disk (RLE+zstd, 0.04 B/voxel), 59 MB on the network, 488 MB in memory row-packed
  (0.49 B/voxel) vs 2973 MB unpacked — [zeux.io, Voxel terrain: storage](https://zeux.io/2017/03/27/voxel-terrain-storage/).
- Star Citizen Planet Tech v4 (CitizenCon 2949): terrain from temperature and humidity; what you see from orbit
  matches the ground — [PC Gamer](https://pcgamer.com/star-citizens-procedurally-generated-planets-showcased-at-citizencon);
  caves are generated points of interest — [starcitizen.tools](https://starcitizen.tools/Update%3AStar_Citizen_Release_1.0).
  No primary source on the representation; heightfield planet with authored cave POIs is **unverified**.
- Outerra: arbitrary-resolution elevation data refined "to centimeter resolution using fractal algorithms", LOD "from
  thousands of kilometers down to centimeters" —
  [Game Developer: Outerra](https://gamedeveloper.com/business/-i-outerra-i-a-seamless-planet-rendering-engine);
  [ModDB](https://www.moddb.com/engines/outerra/articles).
- CDLOD: quadtree of regular grids, one distance-based LOD function over the whole mesh, vertex morphing between
  levels; Strugar, JGGGT 14(4) 2009 — [CarsF/CDLOD](https://github.com/CarsF/CDLOD),
  [tschie/terrain-cdlod](https://github.com/tschie/terrain-cdlod).
- Cube-to-sphere mapping: subdivide cube faces into grids and project to the sphere; chunked LOD per face —
  [acko.net, Making Worlds 1](https://acko.net/blog/making-worlds-1-of-spheres-and-cubes/). Seed of Andromeda:
  mapping cubic voxels onto a sphere "cannot be done without distortion", severe near cube-face edges in its old
  scheme — [ModDB dev summary](https://www.moddb.com/news/seed-of-andromeda-dev-summary-21st-march-2015).
- GPU Gems 3 ch. 1 (Geiss): density evaluated per 32×32×32-cell block into a 3D texture, marching cubes on the GPU,
  overhangs allowed — [NVIDIA GPU Gems 3 ch. 1](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu).
- Quilez: "bound" SDFs return only a lower bound of the true distance; smooth min cannot be exact; analytic noise
  derivatives sped up terrain raymarching — [distfunctions](https://iquilezles.org/articles/distfunctions),
  [smin](https://iquilezles.org/articles/smin), [raymarching SDFs](https://iquilezles.org/articles/raymarchingdf).

### Inferences
- The source of truth should be a density function of body-local position in meters plus a sparse set of edits.
  Storage of unedited terrain is zero (SE, Roblox's RLE, Zylann's full load mode all agree that only edits are data).
- A heightfield on a sphere (cube-face or analytic) is the cheapest way to express planet-scale relief, and it gives
  per-region min/max elevation bounds that let the volumetric octree skip empty and solid chunks. Use it inside the
  generator, not as a second renderer.
- Body-local Cartesian voxels (Planet Nomads, SE) avoid Seed of Andromeda's sphere-mapping distortion; building
  grids already need local tangent frames (00-scope §11).
- Rendering voxels directly (SVO/brick-map ray marching, Teardown-style) fights ADR 0039/0040 (raster PBR, visibility
  buffer) and produces no triangle colliders; reject for terrain.
- Gas and ice giants simply have no terrain component.
- Water is a level (sea-level sphere, per-basin lake levels), not voxels; dynamic water is (c) in 00-scope.

### Gaps
- No primary source for NMS region/chunk sizes, voxel size or LOD depth (talk video not transcribed).
- No primary source for Astroneer, Deep Rock Galactic, Enshrouded or Star Citizen terrain internals.
- Teardown (voxel destruction at ~10 cm) not researched; it renders voxels by ray marching, not relevant to
  raster terrain (**unverified**).

## 2. Meshing and seams

### Takeaway
Marching cubes with Lengyel's Transvoxel transition cells is the proven crack-free LOD scheme with the least coupling
between chunks (Godot Voxel Tools, Unreal Voxel Plugin, C4). Dual methods (surface nets, dual contouring, dual
marching cubes) give fewer vertices and better triangles (NMS moved to DMC for exactly that) but need their own seam
handling (overlap at a lower isolevel, skirts, or meshing a shared boundary strip).

### Cited Findings
- Transvoxel: "a method for seamlessly stitching together neighboring triangle meshes generated from voxel data at
  differing resolutions"; considers "nine samples of the high-resolution data" giving "512 cases" in "73 equivalence
  classes"; applies between one resolution and "exactly half that resolution"; "free of patent claims"; Lengyel's
  dissertation (UC Davis 2010) and "Transition Cells for Dynamic Multiresolution Marching Cubes" (JGGGT 2010) —
  [transvoxel.org](https://transvoxel.org/). The tables are on Lengyel's GitHub; their license was not stated on the
  page (**check before use**).
- The Rust `transvoxel` crate: adjacent blocks "can only either have the same resolution, or one have double the
  resolution of the other"; LOD management is left to the caller — [docs.rs transvoxel](https://docs.rs/transvoxel).
- Godot Voxel Tools (Zylann): Transvoxel mesher on SDF; transition meshes toggled in the vertex shader with a 6-bit
  `u_transition_mask` and a secondary vertex position; "seams may still be visible as occasional sharp little steps";
  inactive transitions hidden by collapsing vertices to degenerate triangles; LOD i→i+1 doubles voxel and block size
  at the same block resolution; octree streaming (one viewer) or clipbox streaming (multiple viewers); optional LOD
  fade by dithered discard; detail normal maps for distant meshes, GPU-generated only for unedited voxels; "Only
  full-resolution voxels can be edited"; full load mode memory grows with edited chunks; SDF stored as 16-bit
  fixed point by default (±500, step ~0.015), 8-bit gets blocky at coarse LODs; noise is "often the main bottleneck"
  — [Voxel Tools: smooth terrain](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/).
- Zylann on seams: he tried the NMS overlap trick with Transvoxel and got Z-fighting; uses marching-squares skirts
  in another implementation — [issue #24](https://github.com/Zylann/godot_voxel/issues/24).
- Unreal Voxel Plugin: smooth mode is marching cubes with Transvoxel for LOD; collisions depend on a "Max Collisions
  LOD" property; Chaos cooks collision 4.7× faster than PhysX (release 1.2) —
  [UE forum thread](https://forums.unrealengine.com/t/voxel-plugin/98080),
  [80.lv: Voxel Plugin 1.2](https://80.lv/articles/voxel-plugin-1-2-released/?amp=1).
- Dual contouring (Gildea): QEF per leaf from edge crossings and normals, fall back to the mass point outside the
  cell; octree simplification by summed QEFs; OpenCL only for "voxel discovery", octree built on CPU because "tree
  traversal maps poorly to GPUs"; QEF math and noise were the bottlenecks; self-intersections handled by a
  post-simplification pass, not the "no intersections" variant —
  [ngildea: implementing dual contouring](https://ngildea.blogspot.com/2014/11/implementing-dual-contouring.html).
- References on surface nets, manifold DC, cubical marching squares, DMC, seamless LOD collected in
  [godot_voxel issue #24](https://github.com/Zylann/godot_voxel/issues/24): Boris the Brave
  [DC tutorial](https://www.boristhebrave.com/2018/04/15/dual-contouring-tutorial/), 0fps
  [smooth voxel terrain part 2](https://0fps.net/2012/07/12/smooth-voxel-terrain-part-2/), Bonsairobo
  [surface nets deep dive](https://bonsairobo.medium.com/smooth-voxel-mapping-a-technical-deep-dive-on-real-time-surface-nets-and-texturing-ef06d0f8ca14),
  dexyfex [seamless LOD](https://dexyfex.com/2016/07/14/voxels-and-seamless-lod-transitions/) (not fetched).
- Roblox's newer mesher is "a dual-style mesher inspired by Dual Contouring and Naive Surface Nets" (SIGGRAPH 2020,
  slides rasterized, content not extracted) — [zeux.io siggraph2020.pdf](https://zeux.io/data/siggraph2020.pdf)
  (via search snippet).

### Inferences
- Meshes are caches: they are never saved or sent, so the mesher can change later. Put it behind an `IsoMesher`
  interface (as SE does) and pick the cheapest crack-free one first.
- Transvoxel's property that matters most for Veach: a chunk's mesh is a pure function of (chunk key, which faces
  border a coarser neighbor, generator, edits). Each chunk can be meshed by any worker with no neighbor data
  dependency, which fits jobs, determinism and headless tests.
- Coarse-grid samples coincide with every other fine-grid sample in an octree of same-resolution chunks, so edits
  can propagate to coarser levels by subsampling, with no blending rule to invent.
- Surface nets is the main challenger: fewer vertices (~1 per surface cell), simpler, GPU-friendly, but no standard
  LOD seam. A spike can compare both cheaply.

### Gaps
- No published per-chunk meshing times for any shipped title. CPU cost per 32³ chunk is my estimate (derived, below).
- License of Lengyel's Transvoxel tables not confirmed.

## 3. Edits

### Takeaway
Shipped games persist edits as voxel data for edited regions only (SE, Roblox, Planet Nomads' R*-tree, Zylann's
edited blocks). Edits are applied at full resolution only (Zylann). None publishes how edits survive generator
changes; NMS's policy is to never change terrain generation for existing planets.

### Cited Findings
- SE persists only player changes — [Rosa 2014](https://blog.marekrosa.org/2014/12/space-engineers-super-large-worlds_17/).
- Planet Nomads stores modifications in an R*-tree in SQLite, indexed geographically —
  [Game Developer](https://gamedeveloper.com/programming/bending-unity-to-carry-spherical-voxel-planets-in-planet-nomads).
- Roblox: two-stage writes (mark rows, then write cells); undo stores the affected box RLE-packed before each
  operation; network sends 16³ chunks on join and 4³ for updates —
  [zeux.io storage](https://zeux.io/2017/03/27/voxel-terrain-storage/).
- Zylann: only full-resolution voxels can be edited; edited voxels feed detail normal maps —
  [smooth terrain](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/).
- NMS Origins: "Terrain generation for existing planets has not been affected, and player bases have not been
  reset" — [nomanssky.com Origins](https://nomanssky.com/origins-update/) (from 00-scope notes).

### Inferences
- Two forms are needed: brush operations (small, for the wire and replay) and materialized override samples (for
  saves and for chunks a late joiner loads). Ops that read the current field (smooth, blend) depend on the generator
  being deterministic, so the server must be able to send materialized chunks when hashes disagree.
- Absolute overrides (store the edited value) survive a generator change better than additive deltas (a delta
  re-applied on new terrain moves with it), but both need the generator version pinned for edited bodies.

### Gaps
- No primary source on edit sync protocols in SE, NMS or Astroneer.

## 4. Chunk streaming, LOD depth, scale range

### Takeaway
Octree LOD depth grows with log2 of the body's diameter over the LOD0 chunk edge: about 7 levels for a 1 km asteroid
and 21 for Earth at s = 1, so the s range costs about 3.3 levels per factor of 10 in radius. Per-level work is
roughly constant, so descending from orbit costs a number of chunk generations proportional to the level count, and
the streaming rate during flight is proportional to speed over altitude.

### Cited Findings
- Zylann: LOD i+1 doubles voxel and block size, fixed block resolution; with block size 16, 4 LODs cover 256 units,
  each extra LOD doubles the extent — [smooth terrain](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/).
- Planet Nomads octree chunks span 16 m to 128 km for a 120 km planet —
  [Game Developer](https://gamedeveloper.com/programming/bending-unity-to-carry-spherical-voxel-planets-in-planet-nomads).
- GPU Gems 3 and Roblox both use 32³ blocks for generation/storage —
  [GPU Gems 3 ch. 1](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu),
  [zeux.io](https://zeux.io/2017/03/27/voxel-terrain-storage/).
- Planet Nomads: 2 km visibility, 120 km/h vehicles, generation must keep up —
  [Game Developer](https://gamedeveloper.com/programming/bending-unity-to-carry-spherical-voxel-planets-in-planet-nomads).
- Elite normal flight ~300 m/s with supercruise near bodies — 00-scope (from
  [Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe)).

### Derived numbers (mine; to be checked by spikes)
Assumptions: LOD0 cell c0 = 0.5 m, chunks of 32³ cells (edge e0 = 16 m), level l has edge e_l = 16·2^l m. A chunk
splits when the camera is closer than λ·e_l (λ = LOD distance ratio). Shell thickness t ≈ 1.5 chunks (relief and
caves make the surface cross more than one chunk layer).

- **Depth** L = ceil(log2(2R / e0)); levels = L + 1:

  | Body | 2R | Levels |
  |---|---|---|
  | Asteroid, 1 km | 1 km | 7 |
  | Asteroid, 10 km | 10 km | 11 |
  | Earth at s = 0.01 | 127 km | 14 |
  | Earth at s = 0.1 | 1,274 km | 18 |
  | Moon at s = 1 | 3,474 km | 19 |
  | Earth at s = 1 | 12,742 km | 21 |
  | Super-earth 2 R⊕ at s = 1 | 25,484 km | 22 |

- **Chunks per active level** ≈ 3πλ²·t (an annulus from λe to 2λe, in chunk units): λ = 4 → ~226; λ = 6 → ~509.
- **Pixel error link:** at 1080p and 60° vertical FOV a cell of size c at distance d covers ≈ 935·c/d pixels, so
  λ ≈ 29/p for a target of p pixels per cell at the split distance: λ = 4 ↔ p ≈ 7; λ = 6 ↔ p ≈ 5.
- **Active levels from the ground** ≈ log2(view distance / (λ·e0)) + 1: with 100 km visible (mountains at s = 1),
  λ = 4 → 11 levels → ~2,500 resident chunks; λ = 6 → ~10 levels → ~5,100.
- **Memory** with a compact 16-byte vertex and meshlets (~1,300 vertices, ~2,400 triangles per surface chunk):
  ~40 KB per chunk → ~100 MB (λ = 4) or ~200 MB (λ = 6) of terrain mesh.
- **Triangles** ~2,400 per chunk → 6 M resident at λ = 4, about a quarter in the frustum (~1.5 M), before occlusion.
  Distance-based LOD over-tessellates grazing ground; that is the cost of isotropic LOD.
- **Streaming rate** while moving at speed v at altitude h: N ≈ 4λt·v / e_min, e_min = max(e0, h/λ).
  - Near the ground (h < λe0 = 64 m), λ = 4: N ≈ 1.5·v chunks/s (v in m/s): 150/s at 100 m/s, 450/s at 300 m/s.
  - Above that: N ≈ 4λ²t·v/h = 96·v/h (λ = 4). A budget of B chunks/s allows v_max ≈ B·h/96: speed proportional to
    altitude, the same shape as Elite's supercruise.
- **Descent from orbit** needs about one ring per level: ≈ levels × 3πλ²t generations: ~4,700 at s = 1 (λ = 4) and
  ~3,200 at s = 0.01. At B = 300/s: ~16 s and ~11 s of streaming, spread over the descent.
- **CPU cost per chunk** (estimate, **unverified**): 33³ ≈ 36k samples; SIMD noise at ~5 ns per octave-sample and
  10 octaves ≈ 1.8 ms; a heightfield fast path (33² columns, 3D terms only in the cave band) ≈ 0.1-0.5 ms; marching
  cubes ≈ 0.5 ms. So ~1-3 ms per chunk per P-core, ~300-1,000 chunks/s per core.
- **GPU cost per chunk** (estimate): ~1,000 flops/sample × 36k = 36 MFLOP; at a third of M1's 2.6 TFLOPS that is
  ~0.05 ms, ~20,000 chunks/s of GPU time before meshing and memory traffic.

### Inferences
- Chunk counts and memory are dominated by λ, not by the radius; the radius only adds levels (and each level is a
  roughly constant ring). The scale range costs log, not linear.
- LOD0 cell size must be a universe constant (manifest), not a quality setting, because collisions and edits must be
  identical on every machine. λ is the quality knob.
- The binding streaming case is fast flight close to the ground, not descent from orbit.

### Gaps
- No measured meshing throughput on Apple silicon; the spike must measure it on the M5 Max and scale.

## 5. GPU-meshed chunks to CPU physics; determinism

### Takeaway
Roblox and Voxel Plugin generate collision from the polygon mesh on the CPU, lazily and per small chunk. Veach's
determinism contract (00-scope, Q12) says GPU output is visual-only, and a dedicated server or headless test has no
GPU (null RHI), so collision geometry must come from a CPU mesher.

### Cited Findings
- Roblox: physics uses the mesher's polygons ("we decided to use the polygonal representation for collision"), 8³
  physics chunks, broadphase bitmasks per chunk (1 bit solid, 1 bit water), narrowphase data built lazily when a
  contact or raycast needs it and cached; incremental updates judged "too complex", chunks are rebuilt on any edit;
  Bullet BVH build was ~3-5× slower than mesh generation, their kD tree ~equal to meshing; ~24.5 bits per triangle for
  mesh plus tree — [Roblox: voxel terrain physics](https://about.roblox.com/newsroom/2018/01/voxel-terrain-physics).
- Jolt: `MeshShape` is static, non-convex and "costly to simulate"; heightfields recommended for grid terrain;
  sanitizing removes degenerate triangles; `mMaxTrianglesPerLeaf` defaults to 8; unreleased notes: ~25% faster
  build and 1000× fewer allocations (contention when building from several threads) —
  [MeshShapeSettings](https://jrouwe.github.io/JoltPhysics/class_mesh_shape_settings.html),
  [release notes](https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html),
  [jolt-jni shape guide](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/shape.html).
- A Jolt user with 128×128 chunks of 256-2048 triangles called Jolt "unusable in big static worlds" (complaint, not a
  measurement) — [Jolt issue #1081](https://github.com/jrouwe/joltphysics/issues/1081) (via search).
- Voxel Plugin: Chaos cooks collision 4.7× faster than PhysX —
  [80.lv](https://80.lv/articles/voxel-plugin-1-2-released/?amp=1).
- Veach RHI: no non-blocking completion query (VCH-073); readback safe only when a frame slot comes round
  ([ADR 0030](../../adr/0030-frame-and-sync-model.md)); no indirect draw (VCH-071).

### Inferences
- Colliders: CPU mesh of LOD0 chunks in a physics radius around each dynamic body, built on worker threads; heightfield
  shapes are impossible with caves.
- Determinism reduces to: the quantized samples are identical. That is Q12's generator contract plus a mesher
  compiled without FMA contraction.

### Gaps
- No measured Jolt MeshShape build time per chunk (Q9's spike should measure it).

## 6. Cost: M1 vs M3 Pro; RHI gaps; headless and MCP

### Cited Findings
- M1 8-core GPU: 2.6 TFLOPS FP32, ~68 GB/s; M3 Pro 18-core GPU: 6.4 TFLOPS (single source), ~150-154 GB/s —
  [nanoreview](https://nanoreview.net/en/gpu-compare/apple-m3-pro-gpu-18-core-vs-apple-m1-gpu-8-core),
  [AnandTech M3](https://www.anandtech.com/show/21116/apple-announces-m3-soc-family-m3-m3-pro-and-m3-max-make-their-marks).
- CPU core counts: M1 4 performance + 4 efficiency; M3 Pro 12-core is 6 + 6 (**unverified**, Apple spec pages not
  fetched).
- Veach: single-threaded, job system is VCH-015 (`ARCHITECTURE.md`, backlog); the renderer draws meshes only from
  `MeshAsset` (grep of `engine/render/src`); MCP's tool set is fixed in the engine, with no module registration
  ([mcp-server spec](../../specs/mcp-server/spec.md)).

### Inferences
- CPU meshing blocks on VCH-015 (threads), not on any RHI gap. GPU meshing blocks on VCH-071 (indirect draw, or
  stall-prone count readback) and VCH-073 (non-blocking readback); 3D textures (VCH-072) are optional (storage buffers
  suffice); async compute (VCH-074) only smooths hitches. Detail normal maps and streamed material maps need VCH-076.
- The renderer needs a runtime-mesh seam (meshes not from assets) and MCP needs module-registered tools or terrain
  inspection through `eval`.
