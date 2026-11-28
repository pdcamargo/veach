# Voxel terrain: bodies, streaming, edits and colliders

`engine/voxel` (`veach::voxel`) turns a density field into streamed, editable, collidable terrain: a planet, an
asteroid or a flat world ([spec](../specs/voxel-terrain/spec.md); ADRs [0053](../adr/0053-terrain-density-field.md),
[0054](../adr/0054-terrain-edits-and-water.md), [0074](../adr/0074-amendments-from-sp-08.md),
[0077](../adr/0077-amendments-from-sp-12.md), [0079](../adr/0079-amendments-from-sp-09.md),
[0083](../adr/0083-terrain-colliders-with-an-adjacency-ring.md), [0086](../adr/0086-amendments-from-building-voxel.md)).
It links `core` and `procgen` only. `engine/runtime`'s `TerrainModule` puts bodies in a world and on screen; physics
receives collider meshes as data.

## The lattice

A body has its own integer lattice, in its reference frame: point (i, j, k) is at (i, j, k) × 0.5 m. Everything
canonical is in lattice units: chunk keys, sample positions, brush centers (millimeters), mesh vertices (lattice units
× 2¹⁶, `ChunkMesh::fixed`).

| | |
|---|---|
| LOD0 cell | 0.5 m (`lod0_cell_meters`) |
| Chunk | 32³ cells, at level l cells of 0.5·2ˡ m: 16 m at level 0, 262 km at level 15 |
| Samples | 35³ per chunk (the apron gives gradients), int16 companded distance + u8 material |
| Key | `ChunkKey{level, x, y, z}`; the chunk's origin is (x, y, z) × 32·2ˡ lattice units |

A sample is a pure function of its lattice point, never of the chunk or level asking: two chunks read the same bits at
the points they share, and Transvoxel's seams close.

## Density from a field program

```cpp
#include <veach/voxel/terrain.hpp>
namespace vx = veach::voxel;
namespace pg = veach::procgen;

auto graph = pg::parse_field_graph(json, "planet.field.json");
auto program = pg::compile(*graph, {.seed = body_seed, .planet_m_per_m = 1.0 / s_p});
auto density = vx::ProgramDensity::make(std::move(*program), {
    .shape = vx::BaseShape::sphere,   // d = |p| - radius_m + density_scale * output
    .radius_m = 637'100.0,
    .density_output = "density",      // meters; negative is rock
    .density_scale = 1.0,
    .material_output = "material",    // rounded to 0..255; omit for a constant `material`
});
```

- **Shapes.** `sphere` (planets, asteroids: centered on the lattice origin), `plane` (`p.y - height_m`: a flat world,
  +Y up) or `none` (the program is the whole density: caves, floating islands).
- **One chunk per call.** `sample(key, faces, out)` evaluates the grid and the full-resolution faces with procgen's
  interpreter on the calling thread (one chunk per job, [ADR 0084](../adr/0084-amendments-from-building-procgen.md)).
- **Bounds.** `classify(box)` proves a box all air or all rock from `procgen::lattice_bounds` and
  `lattice_variation`, splitting it up to `classify_depth` (6) times. Streaming never samples those chunks. The
  bounds are proven, so they are loose: keep the graph's octaves honest (an amplitude of 30 m that only ever reaches
  10 m costs sampled chunks with nothing in them).
- `distance(x, y, z)` gives the unquantized field at any point, for tools and flight (find the ground by bisection).

Any other source implements `vx::DensitySource` (`sample`, `classify`, `encoding`); `vx::FieldSource` wraps a C++
`DensityField` (the spikes' fields).

## A terrain body

```cpp
vx::TerrainSettings settings{.octree = vx::OctreeBounds::around_origin((*density)->max_extent() + 16.0),
                             .lambda = 4.0};
settings.budget = vx::TerrainStreamingBudget::from_rate(measured_chunks_per_second);
auto body = std::make_shared<vx::TerrainBody>(*density, settings);

body->set_viewers(viewers);                    // body-local meters, any number
const vx::StreamStats stats = body->update(jobs);  // once per frame
```

Each `update`:

1. collects the chunk jobs the previous update launched (they ran across the frame on `veach::JobSystem`);
2. selects the restricted octree: a node splits when a viewer is within λ child-chunk edges of it, stays split until
   every viewer is beyond λ·(1 + hysteresis), and boxes proven empty or solid are pruned;
3. launches up to `budget.jobs_per_frame` chunk jobs for what the selection lacks, coarse levels first: sample,
   overlay edits, mesh with Transvoxel, hash, run the payload hook;
4. swaps the drawn set to the selection only when every chunk of it is meshed (never a hole). While the drawn set lags
   more than `budget.patience` updates, the effective λ drops toward `budget.min_lambda`; it recovers once the stream
   keeps up. The simulation never waits on terrain.

Read the result as data: `drawn()` (what to draw), `ready()` and `evicted()` (this update's arrivals and departures),
each a `ChunkRef`: an immutable record with the slot (key and finer-face mask), the mesh, its hash and the payload.
A chunk whose neighbor changes level gets a new slot and is remeshed with transition cells on its finer faces.

**Budgets.** Set them from measured chunks/s per machine, not a formula (ADR 0079 rule 2). `veach-bench-terrain`
measures it on SP-09's 100 m benchmark; on the M5 Max (release preset) with a 19-octave planet:

| Threads | Chunks/s | Sample ms per chunk | Mesh ms per chunk |
|---|---|---|---|
| 1 | 213 | 4.64 | 0.065 |
| 4 | 824 | 4.71 | 0.067 |
| 17 | 2,786 | 5.90 | 0.102 |

`asynchronous = false` makes `update()` wait for its own jobs (tests, benchmarks, a server ticking terrain directly);
the selection then depends only on the viewer path and the budget, never on the worker count.

## Edits

```cpp
vx::BrushOp op{.kind = vx::BrushKind::subtract, .center_mm = {x_mm, y_mm, z_mm}, .radius_mm = 2'000};
veach::Result<vx::EditResult> r = body->apply(op, jobs);   // seq 0: the next number
```

- `add@1` and `subtract@1` spheres, radius 1 mm to 16 m, in integer arithmetic: the same samples on every machine.
  A bad op is `voxel.brush_invalid` and changes nothing. Ops apply in sequence order.
- `apply` waits for jobs in flight, writes the owned samples of every LOD0 chunk within the radius plus 2 cells
  (sampling a chunk the first time an op touches it, even one the bounds called solid), and marks every held chunk
  and collider that may read them for remeshing. Drawn chunks stay drawn until their new mesh lands.
- `edits()` is the store: one `OverrideRecord` per edited LOD0 chunk (its 32³ owned samples, a mask of what ops
  wrote, the base hash of the generated samples) and the `journal()` of ops since the last `fold_journal()`.
  `hash()` and `record_hash()` are canonical: compare them between peers.
- Chunks of every level read the records at their own lattice points, so a dig shows at coarse levels exactly where
  their points fall inside it.
- `restore(record)` puts a saved record back over regenerated samples; a different base hash logs
  `voxel.base_mismatch` and applies it anyway (overrides are absolute).

## Colliders

```cpp
struct MyPhysics : vx::ColliderSink {
    void submit(vx::ColliderUpdate update) override { /* cook on workers, install before update.install_tick */ }
};
body->set_collider_sink(&physics);
body->update_colliders(probes, tick, jobs);   // probes: position and velocity of dynamic bodies, body-local
```

Each call picks the LOD0 chunks within `collider_reach_m` (32 m) of each probe's path over `collider_lookahead_s`
(0.25 s), meshes them and their neighbors on jobs, and submits the chunks whose mesh or ring changed, for install
before `tick + collider_delay_ticks` (2), in key order, plus the chunks no probe needs. A `ColliderMesh` holds
lattice positions (welded), its own triangles first (`own_triangles`), then the neighbors' triangles that touch its
box: adjacency only, they must never produce contacts (ADR 0083). A scene whose origin is a LOD0 grid point converts
positions exactly. Swapping a chunk's shape must wake the bodies over it (ADR 0077 rule 5). The install itself is
`engine/physics`'s (VCH-113): `veach::TerrainColliders` in `engine/runtime` is the `ColliderSink` that hands the
updates to a physics scene ([physics guide](physics.md#terrain-colliders)).

## In a world: TerrainModule

```cpp
engine.import<veach::TerrainModule>();
auto& terrain = veach::services(world).get<veach::TerrainBodies>();
const veach::Entity planet = world.entity("planet").set(veach::ReferenceFrame{.up = veach::FrameUp::radial});
terrain.add(planet, body, {.materials = {lowland, highland, rock}});   // MeshStore materials by voxel material id
camera.child_of(planet).add<veach::TerrainViewer>();
```

- A body lives in a reference frame: its lattice is the frame's coordinates. Viewers are the `TerrainViewer`
  entities, their `FrameTransform` converted into the body's frame in double every frame (`frame_pose`).
- Chunk render meshes are built on the chunk's job (the payload hook), added to the `MeshStore` on the main thread in
  a fixed order (deterministic ids) and removed when evicted. Extraction places each chunk at its origin, converted
  from lattice integers to the frame's meters and then to render space relative to the render origin in double; the
  vertices are chunk-relative floats.
- Without a renderer (a server), the module still streams; nothing is uploaded.
- `TerrainBodies::stats(frame)` has the last update's `StreamStats`.

## Checking your terrain

- **Weld check.** `body->weld()` counts edges used by one triangle across the drawn set (by lattice position): a crack
  between chunks is an open edge. It must be 0; the lab's descent logs it at every checkpoint.
- **The lab's terrain view:** `VEACH_TERRAIN_OUT=build/terrain veach-lab --rhi metal --headless --frames 20000
  --script scripts/terrain_descent.luau` flies from orbit to 10 m, logs each checkpoint and writes screenshots.
- **Hashes.** `mesh_hash`, `samples_hash`, `TerrainBody::drawn_hash` and the store's hashes are canonical: commit
  them in tests, as `tests/voxel_terrain_tests.cpp` does, so Windows CI checks them too.

## Errors

`voxel.density_invalid` (a missing program output, a bad setting), `voxel.brush_invalid` (an op out of range or out
of order), `voxel.base_mismatch` (a restored record's base changed): [errors.md](../reference/errors.md).
