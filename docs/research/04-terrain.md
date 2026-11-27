# 04: Editable volumetric terrain: representation, meshing, LOD, streaming, edits, physics

How planet and asteroid terrain is represented, meshed, LODed, streamed, edited (caves included) and handed to CPU
physics, across bodies whose radii differ by five orders of magnitude and at every creator scale s, while edits and
detail stay in meters. Raw notes and every source: [notes/04-terrain-notes.md](notes/04-terrain-notes.md). Numbers
marked **(derived)** are my arithmetic, shown in the notes so the spikes can check them; **(unverified)** means I
could not confirm it from a primary source.

**Summary.** Terrain is one signed density field per body, a pure function of body-local position in meters, plus
sparse edits. It is meshed volumetrically at every LOD by a body-local octree of 32³-cell chunks, with marching cubes
and Transvoxel seams. The CPU mesher is authoritative: it builds the colliders and works on the null RHI. GPU meshing
comes later as a visual-only accelerator for far chunks. A heightfield on the sphere is an ingredient of the
generator and a way to skip empty chunks, not a second renderer. Edits are deterministic brush operations, stored as
absolute override samples per LOD0 chunk. Most of the cost does not depend on radius: a larger body adds octree
levels (about 3.3 per factor of 10 in radius), and each level costs about the same.

## 1. The problem in Veach's terms

What the scope ([00-scope.md](00-scope.md)) requires of Q4 (its (a) items): bodies without surfaces, asteroids,
water bodies, caves, building frames on a sphere, a chunk-ready hook for derived data, and a streaming rate that is a
function of Q10's approach speed. On top of that, from the brief and the repository:

- **Range of bodies.** From a 1 km asteroid (never scaled: 00-scope §3) to a super-earth of 12,700 km radius at
  s = 1. Earth at s = 0.01 has a 63.7 km radius. Gas and ice giants have no terrain at all.
- **Two length regimes in one field.** Planet-scale features (continents, mountain ranges, basins) follow s_p.
  Detail (rocks, cave tunnels, overhangs, river banks, every edit brush) is in meters and never scales. The generator
  must be split along that line (00-scope §4), and the terrain system must hold both regimes at once: a 0.5 m cell
  and a 524 km cell on the same body.
- **No loading screen** from orbit to the surface, then landing and digging a hole: the first slice.
- **Determinism** ([ADR 0018](../adr/0018-engine-lifecycle-and-phases.md); 00-scope Q12): anything gameplay or
  the server depends on, colliders included, must come from a deterministic path. GPU output is visual-only unless
  it is proven bit-equal on Metal and WARP.
- **Headless first** (non-negotiable 8): a dedicated server, CI and record/replay run on the null RHI, which does
  nothing on the GPU. Colliders and edits must exist there.
- **Today's engine.** Single-threaded (job system is VCH-015). The renderer draws only cooked `MeshAsset`s
  ([ADR 0011](../adr/0011-gltf-import-and-cooking.md)) through the interim visibility buffer
  ([ADR 0040](../adr/0040-interim-realtime-path-visibility-buffer.md)): meshlets, vertex pulling, one draw per
  instance, no indexed or indirect draws. The RHI has compute, storage buffers, 32-bit atomics and per-slot readback,
  but no indirect dispatch or draw (VCH-071), no 3D textures or arrays (VCH-072), no non-blocking completion query
  (VCH-073), no async compute (VCH-074), no texture atomics or 64-bit atomics (VCH-075), and no texture updates
  after creation (VCH-076) ([rhi-capabilities](../reference/rhi-capabilities.md)). The render graph derives all
  barriers ([ADR 0029](../adr/0029-render-graph-sync-and-barriers.md)) and the CPU runs up to `frames_in_flight`
  frames ahead ([ADR 0030](../adr/0030-frame-and-sync-model.md)).
- **Layering** ([ADR 0042](../adr/0042-space-toolbox-layering.md)): generic needs move down into the engine. Chunked
  voxel storage, meshing and LOD streaming are generic, and spheres, s and sea level belong to the toolbox.

## 2. Options, with precedents

### 2.1 Representation

| Option | What it is | Precedents |
|---|---|---|
| **A. Heightfield on a sphere** | Elevation over a cube-sphere quadtree (chunked LOD or CDLOD morphing). | CDLOD ([Strugar 2009](https://github.com/CarsF/CDLOD)); Outerra, km to cm by fractal refinement ([Game Developer](https://gamedeveloper.com/business/-i-outerra-i-a-seamless-planet-rendering-engine)); cube-sphere chunking ([acko.net](https://acko.net/blog/making-worlds-1-of-spheres-and-cubes/)); Star Citizen, with caves as generated points of interest ([starcitizen.tools](https://starcitizen.tools/Update%3AStar_Citizen_Release_1.0); representation unverified) |
| **B. Heightfield + local voxel volumes** | Heightfield everywhere, with voxel volumes spliced in where there are caves or edits. | No clean shipped precedent found. Space Engineers is the reverse: per-face heightmaps feed a volumetric field ([Keen support](https://support.keenswh.com/spaceengineers/pc/topic/28675-please-consider-fixing-the-planet-height-maps)) |
| **C. One density field, meshed volumetrically everywhere** | A scalar field d(p) over the whole body, generated from a function, plus sparse edits. An octree of chunks is refined only near the surface. | No Man's Sky ([GDC 2017](https://gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-)); Space Engineers ([Rosa 2014](https://blog.marekrosa.org/2014/12/space-engineers-super-large-worlds_17/), [2015](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)); Planet Nomads, 120 km planets with 16 m to 128 km octree chunks ([Game Developer](https://gamedeveloper.com/programming/bending-unity-to-carry-spherical-voxel-planets-in-planet-nomads)); Astroneer ([IGF](https://igf.com/astroneer)); Roblox ([zeux.io](https://zeux.io/2017/03/27/voxel-terrain-storage/)); GPU Gems 3 ch. 1 ([NVIDIA](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu)) |
| **D. Sparse voxel octree or brick map, rendered directly** | Voxels ray-marched or ray-traced, with no triangle mesh. | Teardown-style destruction (unverified); SDF ray marching ([Quilez](https://iquilezles.org/articles/raymarchingdf)) |

Within C, the voxel grid can be **body-local Cartesian** (axis-aligned cubes in the body-fixed frame: Planet Nomads,
Space Engineers) or **sphere-warped** (cube faces projected to the sphere, layered radially). Seed of Andromeda found
that mapping cubic voxels onto a sphere "cannot be done without distortion"
([ModDB](https://www.moddb.com/news/seed-of-andromeda-dev-summary-21st-march-2015)).

### 2.2 Meshing

| Mesher | Strengths | Weaknesses | Seam handling | Precedents |
|---|---|---|---|---|
| **Marching cubes + Transvoxel** | Public tables; each chunk is meshed alone; crack-free between levels that differ by a factor of 2; "free of patent claims" | More vertices, sliver triangles, no sharp features | Transition cells: 512 cases in 73 classes, on faces bordering a finer neighbor ([transvoxel.org](https://transvoxel.org/)). Zylann toggles them per frame with a 6-bit mask in the vertex shader | Godot Voxel Tools ([docs](https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/)); Unreal Voxel Plugin ([forum](https://forums.unrealengine.com/t/voxel-plugin/98080)); C4 engine (Lengyel) |
| **Surface nets** | About one vertex per surface cell, good triangles, no tables, GPU-friendly | No standard LOD seam | Skirts, overlap, or a boundary strip meshed with the neighbor's samples | Roblox's newer mesher is "inspired by Dual Contouring and Naive Surface Nets" ([SIGGRAPH 2020](https://zeux.io/data/siggraph2020.pdf)); [0fps](https://0fps.net/2012/07/12/smooth-voxel-terrain-part-2/) |
| **Dual contouring** | Sharp features, octree simplification | Needs normals (Hermite data); QEF solve cost; self-intersections; tree traversal "maps poorly to GPUs" | Seam strips; NMS overlapped blocks at a lower isolevel ([issue #24](https://github.com/Zylann/godot_voxel/issues/24)) | NMS before 2024; [Gildea](https://ngildea.blogspot.com/2014/11/implementing-dual-contouring.html) |
| **Dual marching cubes** | Adaptive; fewer vertices | The most complex to write | Built into the adaptive octree | NMS since Worlds Part I: "reduces vertex count, increases terrain generation speed... saves significant amounts of memory" ([nomanssky.com](https://www.nomanssky.com/worlds-part-I-update/)) |

**Where meshing runs.** On the CPU (Planet Nomads: C++ threads; Gildea: CPU octree), on the GPU (GPU Gems 3; Gildea
used OpenCL only to find surface voxels), or split between them.

### 2.3 Edits

1. **Op log only.** Store brush operations and replay them over the generator. This is the smallest format, and it
   breaks the moment the generator or float math changes.
2. **Additive deltas.** Store edited minus generated per cell. A generator change shifts every edit with the terrain
   under it.
3. **Absolute overrides.** Store the edited value per cell, with a mask. A generator change leaves the edited cells
   intact, with a visible seam at the mask border. Space Engineers keeps "only player changes"; Roblox stores voxels
   absolutely; Planet Nomads stores modifications in an R*-tree.
4. **Ops on the wire plus absolute overrides at rest** (the hybrid this document recommends).

### 2.4 Physics handoff

1. **CPU mesh, CPU collider.** Roblox builds collision from "the polygonal representation", per 8³ chunk, lazily
   ([Roblox](https://about.roblox.com/newsroom/2018/01/voxel-terrain-physics)).
2. **GPU mesh, read back for colliders.** Needs non-blocking readback (VCH-073), puts GPU results on an
   authoritative path, and does nothing on a server without a GPU.
3. **Collide against the density field directly.** A custom SDF shape. Jolt has none built in, so it would be our own
   narrowphase.

## 3. Tradeoffs

### Representation

| | A. Heightfield | B. Height + local voxels | **C. One density field** | D. SVO/brick ray-march |
|---|---|---|---|---|
| Caves, overhangs, arches | No | Only inside spliced volumes | **Anywhere** | Anywhere |
| Dig a tunnel | No | Yes, after a heightfield-to-voxel conversion | **Yes** | Yes |
| Far LOD cost | Lowest (2D tiles) | Lowest | ~2-3× A with the heightfield fast path **(derived)** | Per-pixel marching |
| Complexity | Low | **Highest**: two meshers, a seam between representations, conversion on first edit | Medium: one mesher, one LOD scheme | High, and a second renderer |
| Fits raster + visibility buffer (ADR 0039/0040) | Yes | Yes | **Yes** | No (needs RT or a ray-marcher; RT is optional per ADR 0003) |
| Colliders | Heightfield shape | Mixed | **Triangle meshes** | Custom |
| Asteroids | No (not height-mappable) | No | **Yes, same code** | Yes |
| Scale range | Excellent | Good | **Good: +3.3 levels per 10× radius** | Good |

B looks like the best of both, but caves are wanted everywhere ("underground ecosystems"), and caves everywhere turns
B into C near the surface while keeping A's renderer and adding a seam between them. D does not fit the renderer we
have and gives physics nothing. A cannot dig a hole sideways. **C wins.** Its cost at far LODs is contained by
evaluating the planet-scale heightfield first and computing 3D terms only in the cave band (see 7.2).

### Meshing

| | **MC + Transvoxel** | Surface nets | Dual contouring | DMC |
|---|---|---|---|---|
| Implementation scope | Small (tables + cell loop + transition cells) | Smallest | Large (QEF, Hermite data, intersection fixes) | Largest |
| Crack-free LOD | **Proven** | Needs our own scheme | Needs our own scheme | Built in |
| Chunk independence (any worker, no neighbor meshes) | **Yes** | Only if one side owns the seam strip | Same | No (adaptive tree) |
| Vertex count | Highest | ~Half of MC (unverified) | Low | Lowest |
| Sharp edges (flattened pads) | Beveled at cell size | Beveled | **Crisp** | Crisp |
| Determinism (CPU, fixed op order) | Easy | Easy | QEF solver must be deterministic | Hard |
| GPU port later | Easy (tables, prefix sums) | Easy | Hard | Hard |

Meshes are caches: they are never saved or sent. The mesher is therefore the cheapest decision to reverse, and the
first one should be whichever is crack-free with the least work. That is Transvoxel. Surface nets is the challenger
in spike S4.1.

### Determinism and testability

- **CPU path:** a pure function of chunk key, seam mask, generator version and edits. On the null RHI it can be
  hashed in a unit test, compared across macOS arm64 and Windows x64 in CI, and replayed exactly from a recording.
- **GPU path:** nothing to test on the null RHI. Metal's fast-math default (unverified, 00-scope) and differences
  between vendors make it visual-only by contract.

### Extensibility

Representation C with a generator contract (pure function of position) lets new body kinds, cave styles and biomes
be generator data and nodes (Q5). A and B force a split between "height" and "3D" content, so a creator's new arch
type has nowhere to live.

## 4. Extension points

| What plugs in | Where | Luau | C++ module | Toolbox core only |
|---|---|---|---|---|
| A new body kind with terrain (rocky, icy, metallic asteroid) | Body-kind registry → which generator graph to run | **Yes**: data + graph composition | New nodes | |
| A body kind with no terrain (gas giant, ring, station) | Just no `TerrainBody` component | **Yes** | | |
| Density generator nodes (noise, erosion field, crater field, cave network) | Q5 graph; per-sample code | Composition and parameters only. Per-sample Luau is too slow (16 M samples/s at 450 chunks/s, derived) and not deterministic (libm) | **Yes** | |
| Coarse whole-body stages (plates, drainage, crater placement, cave network graph per region) | Q3/Q5 coarse stages, cached per body | **Yes** (per region, not per sample), with deterministic math from `@veach` | Yes | |
| Materials per voxel (biome → material id) | Generator stage writing the material channel | **Yes** (rules) | Yes | |
| Brush kinds (crater, ramp, tunnel bore, eruption deposit) | Brush registry: a composition of primitive SDF shapes and operators (union, subtract, smooth-min, flatten-to-plane, paint) | **Yes**: composition | New primitive operators | |
| Derived data per chunk (colliders, navmesh, foliage scatter, cover, ore) | `TerrainChunkReady` / `TerrainChunkEvicted` events with a read-only view of samples and mesh | Light consumers | **Heavy consumers** (navmesh, scatter) | |
| A new mesher (surface nets, DMC, DC) | `IsoMesher` interface in the engine voxel module | | **Yes** | |
| GPU acceleration of far chunks | `ChunkProducer` interface (CPU default, GPU visual-only) | | **Yes** (engine) | |
| Water bodies (sea level, lakes) | Per-body fluid levels from Q3's drainage stage | **Yes** (data) | Render: Q8 module | |
| Chunk key layout, LOD0 cell size, sample encoding, edit format | | | | **Core**: frozen by ADRs, part of saves |

Engine vs toolbox ([ADR 0042](../adr/0042-space-toolbox-layering.md)): `engine/voxel` (generic, flat-world-capable)
owns the chunk store, octree LOD selection, the meshers, the edit store, the scheduler, the chunk events and debug
draw. `space/terrain` owns spherical generators, the s_p/meters split, "up" for brushes (radial from the body
center), sea level, and binding a body's frame (Q1) to a voxel volume. A non-space game could use `engine/voxel`
for a flat voxel world.

## 5. Interactions with the other questions

- **Q1 coordinates.** Terrain lives in the body-fixed frame. Chunk origins are integers (chunk index × edge) in that
  frame. Vertices are unorm16 or float32 relative to the chunk origin: at LOD0 a unorm16 step is 0.24 mm, and in a
  coarse chunk 16,777 km on a side it is 256 m against a 524 km cell. Both are fine **(derived)**. Extraction turns
  chunk origin minus camera into a float32 offset, computed in double. Brush centers must never pass through float32
  body-local positions (1 m resolution at 10⁷ m), so Luau needs Q1's position type, not `vector`. Chunk keys:
  `{body id, level u8, x, y, z int32}`; 22 levels need 3 × 22 coordinate bits plus a level, more than 64 bits, so
  the key is a struct, not a Morton u64.
- **Q2 mechanics.** Terrain is body-fixed, so rotation and orbits never touch it. Nothing to sync.
- **Q3 pipeline.** It provides the body record (radius, s_p, composition, sea level, generator versions) and the
  coarse stages, including a per-body elevation min/max pyramid that the octree uses to skip empty and solid chunks.
  Density must be a function of position, never of chunk key; per-chunk seeds are only for discrete objects
  (rocks, ore).
- **Q5 procgen graph.** It defines how density nodes compile to C++ (SIMD) now and Slang later. Planet-scale terms
  have frequencies in units of the scaled radius; detail terms in meters. Every node takes a band limit (the cell
  size), so coarse chunks evaluate only octaves they can represent. That is required against aliasing and makes
  coarse chunks cheaper.
- **Q6 networking.** Brush ops go on the wire with their tick, plus a per-chunk hash after each op. On a hash
  mismatch, or for a late joiner, the server sends the materialized override chunk. Wire positions are integer
  cell coordinates in a chunk; nothing in the format assumes a scale.
- **Q7 simulation LOD.** Eruptions, landslides and impact craters are system edits through the same brush path, so
  they are deterministic and stored the same way.
- **Q8 renderer.** Terrain chunks need a runtime-mesh path into the renderer (today only cooked `MeshAsset`s render).
  Meshlets are built at mesh time with the ADR 0011 layout, so the visibility pass can draw them. Transition faces
  are selected by a 6-bit mask in the vertex-pulling shader. Texturing is triplanar from per-vertex material ids,
  through bindless handles (no texture arrays needed; VCH-072 helps). Detail normal maps for distant chunks need
  VCH-076. When the planet is a few hundred pixels across, Q8's impostor takes over.
- **Q9 physics.** Static triangle-mesh colliders from LOD0 CPU meshes within a physics radius of every dynamic body.
  No heightfield shapes, because of caves. They are rebuilt on edit on a worker. Jolt `MeshShape` is static and
  "costly to simulate" ([Jolt](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/shape.html)), so
  the radius stays small.
- **Q10 travel.** The streaming budget sets a speed limit that is proportional to altitude: v_max ≈ B·h/96 above
  64 m, ≈ B/1.5 m/s near the ground (λ = 4, B in chunks/s; section 7.3). Landing needs LOD0 chunks (and colliders)
  at the touchdown point before contact.
- **Q11 persistence.** Override chunks are binary blobs (RLE + zstd) keyed by body id and LOD0 chunk key. The
  generator version is pinned per edited body. The LOD0 cell size and sample encoding go in the universe manifest.
  Once a body has edits, s is fixed (00-scope §8).
- **Q12 determinism.** The generator uses engine-provided math. The mesher and brush code are compiled without FMA
  contraction (`-ffp-contract=off`, MSVC `/fp:precise`) and use fixed operation order. Brushes use fixed-point
  arithmetic on int16 samples. A CI test hashes chunks on arm64 and x64.

## 6. M1-class vs M3 Pro-class

| | Base M1 (8-core GPU) | M3 Pro (18-core GPU) | Source |
|---|---|---|---|
| GPU FP32 | 2.6 TFLOPS | 6.4 TFLOPS (single source) | [nanoreview](https://nanoreview.net/en/gpu-compare/apple-m3-pro-gpu-18-core-vs-apple-m1-gpu-8-core) |
| Memory bandwidth | ~68 GB/s | ~150 GB/s | [AnandTech](https://www.anandtech.com/show/21116/apple-announces-m3-soc-family-m3-m3-pro-and-m3-max-make-their-marks) |
| CPU P-cores (meshing workers) | 4 (≈3 free for jobs) | 6 (≈5 free) | Apple specs (unverified) |
| Unified memory (common configs) | 8 GB | 18 GB | (unverified) |

What changes **(derived; spike S4.2 measures it)**:

| Quantity | M1 (λ = 4) | M3 Pro (λ = 6) |
|---|---|---|
| Pixels per cell at the split distance | ~7 | ~5 |
| Resident chunks from the ground (100 km view) | ~2,500 | ~5,100 |
| Terrain mesh memory | ~100 MB | ~200 MB |
| Triangles in the frustum before occlusion | ~1.5 M | ~3 M |
| CPU chunk budget B (2-3 ms/chunk, workers minus frame work) | ~300/s | ~600/s |
| Near-ground speed the stream keeps up with | ~200 m/s | ~270 m/s (at λ = 6 a chunk costs more) |
| Orbit-to-ground streaming, Earth at s = 1 | ~4,700 chunks, ~16 s spread over the descent | ~10,700 chunks, ~18 s |

The LOD0 cell size is identical on both, because collisions and edits must not depend on the machine. The quality
knob is λ (and later detail normal maps). On M1 the binding constraint is CPU meshing throughput (low-altitude
speed) and triangle count. On M3 Pro it is mostly triangle count at the higher λ. GPU meshing of far chunks (later)
would raise B about tenfold on both **(derived: ~0.05 ms of GPU per chunk on M1)**. That is the lever if Q10 wants
fast low-level flight.

**RHI gaps by option:**

| Option | Blocked by |
|---|---|
| CPU meshing, all levels (recommended v1) | **VCH-015 (job system)** for throughput; a runtime-mesh renderer seam. No RHI gap. |
| GPU far-field meshing (later) | **VCH-071** (indirect draw; otherwise counts are read back a frame late), **VCH-073** (to know when results are ready without stalling); VCH-074 optional (smoother frames); VCH-072 optional (storage buffers suffice) |
| GPU meshing for colliders | VCH-073, and a bit-equal GPU proof (Q12). Rejected. |
| Detail normal maps, streamed material maps | **VCH-076**; VCH-072 (arrays) helpful |
| GPU voxel edits / GPU hash maps | VCH-075. Not needed in this design. |

**Headless and MCP.** Everything authoritative runs on the null RHI: generation, meshing, edits, colliders and
hashes. Unit tests: crack test (every boundary edge of a chunk mesh has a matching edge in its neighbor or its
transition face), determinism hash per chunk, edit replay hash, LOD-selection invariants (2:1 balance, parents stay
until children are ready). MCP has a fixed tool set today. Terrain can be inspected through `eval` with Luau
bindings (`terrain.sample(body, pos)`, `terrain.stats()`, `terrain.edit(...)`), and module-registered MCP tools
would be a generic engine seam (goes down per ADR 0042), useful beyond terrain.

## 7. Recommendation

### 7.1 Representation

**One signed density field per terrain body, `d(p) = override(p) if edited else generated(p)`,** where p is a
body-local position in meters and negative is solid. The generator is structured, not monolithic:

1. a **planet-scale heightfield** h(direction) on the sphere (radius and relief follow s_p), with a min/max pyramid
   cached per body;
2. **meter-scale detail** (never scaled) added to it;
3. **3D terms** (caves, overhangs, arches) evaluated only inside a cave band [surface − D_cave, surface + overhang
   height];
4. solid below the band, empty above the relief maximum.

Chunks the min/max bounds prove empty or solid are never sampled.

- **Asteroids** are the same component with a fully 3D generator (no heightfield fast path), 7 to 11 levels, small
  enough to keep every coarse level resident.
- **Gas and ice giants** have no `TerrainBody`.
- **Water** is not in the density field: a sea-level sphere per body (radius follows s_p) and lake levels per basin
  from Q3's drainage stage, masked in 2D. Static: holes do not flood, and dry caves under the sea stay dry. The
  generator reads sea level for shore and seabed materials. Q8 renders the water surface.

### 7.2 Chunks, LOD and meshing

- **Body-local Cartesian octree** of chunks of **32³ cells**. **LOD0 cell = 0.5 m** (chunk edge 16 m), a universe
  manifest constant. Level l doubles the cell size. The root covers the body's bounding cube. The octree is restricted
  (neighbors differ by at most one level) and refined by distance with ratio λ.
- **Samples** are int16 fixed point at 1/1024 m, clamped to a narrow band of ±32 m, plus a u8 material: 3 bytes per
  sample. Interpolation places surfaces to millimeters, and the smallest feature is about two cells (1 m).
- **Mesher:** marching cubes with Transvoxel transition faces, behind `IsoMesher`. All six transition faces are
  meshed with the chunk and toggled per frame by the neighbor mask, so a neighbor changing LOD never forces a
  remesh. Normals come from the density gradient, so chunks are meshed with a one-sample apron.
- **The CPU is authoritative and the only producer in v1.** The GPU producer is a later, visual-only accelerator for
  levels ≥ 2, behind the same `ChunkProducer` interface, once VCH-071 and VCH-073 exist.
- **Do not keep density for unedited chunks** after meshing (Space Engineers does the same). Do keep LOD0 samples for
  the chunks around players (fast edits, colliders): 125 chunks × 108 KB ≈ 13 MB **(derived)**.

### 7.3 Streaming

- **One priority queue per body:** missing chunks the camera needs, ordered by screen-space error, finest first near
  the camera. A fixed per-frame budget of chunk jobs, with a `TerrainStreamingBudget` setting per quality tier.
- **Never a hole.** A parent stays drawn until all its children are meshed, and children are dropped only after the
  parent is back. When the queue backs up, the effective λ drops for the next selection: graceful coarsening, never
  stalls.
- **The speed contract Q10 needs:** with λ = 4 and a 1.5-chunk shell, new chunks per second N ≈ 1.5·v near the
  ground and N ≈ 96·v/h above 64 m. Q10 should cap speed at v ≤ B·h/96, i.e. speed proportional to altitude, which
  is what supercruise-style travel does anyway. At B = 300/s, that is ~200 m/s near the ground, ~3 km/s at 1 km
  altitude and ~300 km/s at 100 km. Descending from orbit is cheap (one ring per level, logarithmic in altitude). Fast
  flight near the ground is the expensive case.

### 7.4 Edits

- **Brush operation** = `{kind, shape, center (int64 body-local mm), orientation (quantized), size, strength,
  falloff, material, tick, author}`. Kinds: add, subtract (dig), flatten-to-plane, smooth, raise and lower (along
  radial up, supplied by the toolbox), paint.
- **Applying a brush:** read the current samples (generated, quantized to int16, then overrides), combine them in
  fixed-point integer math, and write absolute override samples with a per-cell mask into LOD0 chunk override grids.
  Every LOD0 chunk the brush touches is affected, apron included.
- **Coarse levels:** an octree's coarse samples are a subset of the fine samples, so the parent override is the child
  sample at the shared position, propagated up until the edit box is smaller than one cell. A 50 m landing pad shows
  from a few km away and vanishes from orbit, as it should.
- **Persistence (Q11):** override grids RLE + zstd per LOD0 chunk. Roblox's numbers suggest 0.04-0.07 bytes per voxel
  on disk for real content ([zeux.io](https://zeux.io/2017/03/27/voxel-terrain-storage/)). The generator version is
  pinned per edited body. Networking (Q6): ops plus chunk hashes; materialized chunks on mismatch and on join.
- **A brush size cap** (product decision 7) bounds the work and the network burst of a single op. Large terraforming
  is many ops.

### 7.5 Physics

CPU LOD0 meshes become static triangle-mesh colliders, per chunk, within a physics radius (default 2 chunks, 32 m,
around each dynamic body's predicted position over the next 0.25 s). They are built on workers and rebuilt on edit.
From brush op to updated collider, the target is the next fixed step for the chunks under the brush **(to measure)**.
The collider is a function of the quantized samples, so it is deterministic wherever the generator is (Q12). The
dedicated server meshes LOD0 around every player and active body, with no GPU.

### What would change my mind

- **S4.2 shows the CPU cannot reach B ≈ 300 chunks/s on the M5 Max** (scaled to M1), even with the heightfield fast
  path. Then the GPU far-field producer moves into the first slice, and VCH-071 and VCH-073 become blockers.
- **Q10 wants fast flight near the ground** (over ~300 m/s below 100 m). Same consequence.
- **Far-field triangle counts or chunk counts break the M1 frame** after occlusion culling. Then levels beyond some k
  switch to a cube-sphere CDLOD heightfield renderer fed by the same generator, with skirts at the handoff. This is
  contained, because the generator and edit store do not change.
- **Surface nets beats Transvoxel by a wide margin in S4.1** (vertex count, meshing time) and a seam scheme works.
  Then switch meshers; meshes are caches.
- **Base building needs crisp edges on flattened pads.** Then dual contouring for LOD0 only, or prefab foundations
  that hide the bevel.

## 8. What is expensive to undo

1. **LOD0 cell size and chunk size (0.5 m, 32³).** Baked into override storage keys, save files, wire messages,
   collider behavior and brush feel. Changing either invalidates every edited body. Manifest constant.
2. **Sample encoding** (int16 at 1/1024 m, ±32 m, u8 material). It is in every save. 256 materials per body is
   probably enough; widening later means migrating saves.
3. **Edit model** (absolute overrides at rest, ops on the wire, pinned generator version). Q6 and Q11 build on it.
4. **Body-local Cartesian octree and chunk keys.** Every edit is keyed by it.
5. **The generator contract:** density is a pure function of body-local meters plus band limit, never of chunk key
   or LOD. Every Q5 node is written against it.
6. **CPU-authoritative collision.** Q9, Q6 and Q12 lean on it.

Cheap to change: the mesher, λ and the streaming scheduler, GPU acceleration, vertex format, detail normal maps, the
far-field renderer.

## 9. Proposed ADR titles

1. Terrain is a signed density field in body-local meters, meshed volumetrically at every LOD
2. Terrain chunks: a body-local octree of 32³-cell chunks with a fixed LOD0 cell size per universe
3. Terrain samples are int16 fixed-point narrow-band distances with a u8 material
4. CPU terrain meshing is authoritative; GPU meshing is a visual-only accelerator
5. Marching cubes with Transvoxel transition faces as the first terrain mesher, behind `IsoMesher`
6. Terrain edits are deterministic brush operations stored as absolute override samples per LOD0 chunk
7. Generic voxel terrain lives in `engine/voxel`; planetary terrain lives in `space/terrain` (applies ADR 0042)
8. Water bodies are per-body and per-basin levels, not voxels
9. Terrain streaming: a chunk-rate budget, screen-error priority, parents stay until children are ready (and the
   speed-over-altitude contract with Q10)

## 10. Spike candidates

Ordered toward the first slice. Each spike is throwaway and sized by scope.

| Spike | Scope (smallest prototype) | Visible success criterion |
|---|---|---|
| **S4.1 Flat mesher bake-off** | A density function on a flat world (heightfield + one 3D noise cave layer); CPU marching cubes + Transvoxel and surface nets + skirts; octree LOD selection around a scripted camera; upload as runtime meshes to the interim renderer; a headless crack test. Needs a runtime-mesh renderer seam (small). | Wireframe screenshot of three LOD rings with no cracks; Tracy ms/chunk and vertex/triangle counts for both meshers; identical chunk hashes in two runs and on the null RHI. |
| **S4.2 Planet octree across scale** | Sphere generator with heightfield fast path and min/max skipping; the same code at Earth s = 0.01 and s = 1 and a 1 km asteroid; scripted headless descent from orbit to 2 m with time-slicing on the main thread (no job system needed for the spike). Needs Q1's camera-relative offsets. | Screenshots at 10 altitudes with no holes; logged per-level chunk counts, queue length and memory vs the table in section 6; chunk rate B measured on the M5 Max and the per-chunk cost split (sampling vs meshing). |
| **S4.3 Dig and flatten** | Brushes (dig sphere, flatten pad) on S4.2's planet; override grids; coarse-level propagation; replay of an op log. | Screenshot of a dug hole and a pad from 2 m, 200 m and 2 km; edit-to-mesh latency in Tracy; replaying the op log gives identical chunk hashes; the same hashes on Windows x64 CI. |
| **S4.4 Collider handoff** (shared with Q9) | Jolt `MeshShape` per LOD0 chunk around a rolling sphere; rebuild on edit. | The sphere rolls into the S4.3 hole and settles in it, headless, with the same final position in two runs; build ms per chunk. |
| S4.5 GPU far field (later, after VCH-071/073) | GPU density + MC for levels ≥ 2, CPU for LOD0-1. | FLIP difference between CPU and GPU chunks below a threshold; B rises about tenfold. |

## 11. Product decisions surfaced

1. **Smallest diggable feature (LOD0 cell).** Recommend **0.5 m** (features ~1 m, surfaces placed to millimeters),
   fixed per universe. 0.25 m costs about 4× the chunks near the player; 1 m feels coarse for caves and pads.
2. **Cave depth and "caves to the core".** Recommend a creator-set cave band in meters (default 500 m below the
   surface, never scaled), with core access only for small bodies (asteroids, small moons) by default. At s = 1,
   Earth's core is 6,371 km down.
3. **Low-altitude speed limit (with Q10).** Recommend speed proportional to altitude above ~64 m, and about 200 m/s
   near the ground as the M1 target, raised on faster tiers.
4. **Edits after a scale change.** Recommend s fixed per universe once any body has edits (as 00-scope suggests).
5. **Water.** Recommend static levels: dug holes do not flood, caves below sea level are dry unless inside the ocean
   mask.
6. **Editable asteroids at once.** Recommend only the nearest few asteroids as terrain bodies; the rest of a field
   as instanced meshes until approached. Detached fragments becoming debris is a later extension (b).
7. **Brush size cap.** Recommend a radius of at most 16 m per op (one chunk edge); larger terraforming is repeated
   ops.
8. **Generator changes on edited bodies.** Recommend the NMS policy: terrain generation of an edited body is pinned
   to its version forever (Q11).

## 12. Sources

Games and engines:

- No Man's Sky: GDC 2017 "Continuous World Generation in No Man's Sky" — https://gdcvault.com/play/1024265/Continuous-World-Generation-in-No,
  write-up https://gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-,
  video https://www.youtube.com/watch?v=sCRzxEEcO2Y; Worlds Part I (DMC) https://www.nomanssky.com/worlds-part-I-update/;
  Origins https://nomanssky.com/origins-update/; DC claim (thesis) https://www.diva-portal.org/smash/get/diva2:1333567/FULLTEXT02
- Zylann, godot_voxel issue #24 (NMS summary, mesher references) — https://github.com/Zylann/godot_voxel/issues/24
- Godot Voxel Tools, smooth terrain — https://voxel-tools.readthedocs.io/en/latest/smooth_terrain/
- Space Engineers — https://blog.marekrosa.org/2014/12/space-engineers-super-large-worlds_17/,
  https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/,
  https://fresc81.github.io/SpaceEngineers/namespace_sandbox_1_1_engine_1_1_voxels.html,
  https://support.keenswh.com/spaceengineers/pc/topic/28675-please-consider-fixing-the-planet-height-maps,
  https://github.com/asrbic/Procedural_Ore_Generator
- Planet Nomads — https://gamedeveloper.com/programming/bending-unity-to-carry-spherical-voxel-planets-in-planet-nomads
- Astroneer — https://igf.com/astroneer, https://www.unrealengine.com/en-US/spotlights/how-system-era-softworks-leveraged-ue4-to-create-astroneer-s-wonderful-universe
- Roblox — https://zeux.io/2017/03/27/voxel-terrain-storage/, https://about.roblox.com/newsroom/2018/01/voxel-terrain-physics,
  https://zeux.io/data/siggraph2020.pdf
- Star Citizen — https://pcgamer.com/star-citizens-procedurally-generated-planets-showcased-at-citizencon,
  https://starcitizen.tools/Update%3AStar_Citizen_Release_1.0
- Outerra — https://gamedeveloper.com/business/-i-outerra-i-a-seamless-planet-rendering-engine
- Seed of Andromeda — https://www.moddb.com/news/seed-of-andromeda-dev-summary-21st-march-2015
- Enshrouded — https://www.pcgameshardware.de/Enshrouded-Spiel-74267/Specials/Test-Release-Steam-Gameplay-1438724/
- Deep Rock Galactic — https://www.unrealengine.com/spotlights/how-ghost-ship-games-found-success-with-deep-rock-galactic?lang=en-US
- Unreal Voxel Plugin — https://forums.unrealengine.com/t/voxel-plugin/98080, https://80.lv/articles/voxel-plugin-1-2-released/?amp=1

Algorithms:

- Lengyel, Transvoxel — https://transvoxel.org/; Rust crate notes — https://docs.rs/transvoxel
- Strugar, CDLOD — https://github.com/CarsF/CDLOD, https://github.com/tschie/terrain-cdlod
- Geiss, GPU Gems 3 ch. 1 — https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu
- Gildea, dual contouring — https://ngildea.blogspot.com/2014/11/implementing-dual-contouring.html
- Boris the Brave, DC tutorial — https://www.boristhebrave.com/2018/04/15/dual-contouring-tutorial/
- 0fps, surface nets — https://0fps.net/2012/07/12/smooth-voxel-terrain-part-2/
- acko.net, cube to sphere — https://acko.net/blog/making-worlds-1-of-spheres-and-cubes/
- Quilez — https://iquilezles.org/articles/distfunctions, https://iquilezles.org/articles/smin, https://iquilezles.org/articles/raymarchingdf

Physics and hardware:

- Jolt — https://jrouwe.github.io/JoltPhysics/class_mesh_shape_settings.html, https://jrouwe.github.io/JoltPhysics/md__docs_2_release_notes.html,
  https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/shape.html
- Apple GPUs — https://nanoreview.net/en/gpu-compare/apple-m3-pro-gpu-18-core-vs-apple-m1-gpu-8-core,
  https://www.anandtech.com/show/21116/apple-announces-m3-soc-family-m3-m3-pro-and-m3-max-make-their-marks

Repository: [00-scope.md](00-scope.md), [rhi-capabilities](../reference/rhi-capabilities.md),
[BACKLOG](../backlog/BACKLOG.md) (VCH-015, VCH-071..076), ADRs [0003](../adr/0003-platforms-and-hardware-target.md),
[0011](../adr/0011-gltf-import-and-cooking.md), [0029](../adr/0029-render-graph-sync-and-barriers.md),
[0030](../adr/0030-frame-and-sync-model.md), [0040](../adr/0040-interim-realtime-path-visibility-buffer.md),
[0042](../adr/0042-space-toolbox-layering.md).
