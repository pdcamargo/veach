# SP-08: mesher bake-off, marching cubes + Transvoxel against surface nets (VCH-084)

Validates the meshing part of [ADR 0053](../../adr/0053-terrain-density-field.md) (32³-cell chunks, 0.5 m LOD0 cells,
int16 narrow-band distances, marching cubes with Transvoxel seams behind `IsoMesher`, CPU authoritative), with the
determinism rules of [ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md) and the area layout of
[ADR 0065](../../adr/0065-engine-areas-for-the-toolbox.md). Research: [04-terrain](../04-terrain.md), spike S4.1.

**Verdict.** The decision holds: keep marching cubes + Transvoxel. It is watertight across every LOD seam the octree
produced (0 open edges out of 789,402 triangles, levels 0 to 4, faces, edges and corners), meshes a chunk in about
0.08 ms on one core, and gives the same hash on 1 and 8 threads, in Debug and in Release. Surface nets does not win:
it has about the same vertex and triangle counts, it is slower as built, and its seams are only hidden, not closed.

The Transvoxel tables are **MIT-licensed** (verified, below), so they are vendored, not re-derived.

The spike found four things the ADRs don't say yet:

1. **The field must return the same sample at a lattice point whichever level asks.** Transvoxel joins a coarse chunk's
   face to the fine neighbor by re-sampling the face at the fine spacing; it only closes if the coarse chunk's own
   corner samples equal the fine ones. A generator that band-limits by the asking chunk's level breaks this (ADR 0053
   says "a pure function of position and band limit").
2. **Toggling transition faces in the shader triples sampling.** Meshing all six faces costs 20 % more mesh time, but each
   face needs 3 layers of 67² half-spacing samples (for normals), so sampling goes from 2.5 to 7.2 ms per chunk.
3. **Chunk-relative float positions are exact only up to level 3.** From level 4 (256 m chunks) the same seam vertex
   lands on different floats in the two chunks (36 of 26,793 shared vertices in one run).
4. **Two renderer-side artifacts** that look like cracks but are not (details under "Screenshots").

## What was built

Throwaway, except possibly the vendored tables and the `IsoMesher` shape.

| Piece | Where | Notes |
|---|---|---|
| `veach::voxel`: `ChunkKey`, `Sample` (int16 at 1/1024 m, ±32 m, plus u8 material), `DensityField`, `ChunkSamples`, `sample_chunk`, `ChunkMesh`, `IsoMesher`, `mesh_hash` | `engine/voxel/include/veach/voxel/voxel.hpp`, `src/samples.cpp` | Links `veach::core` only, never rhi or render (ADR 0065). Canonical: fixed-point vertex positions, integer-only hashes |
| Octree LOD selection | `octree.hpp`, `src/octree.cpp` | Distance-based with ratio λ, restricted (touching leaves within one level, faces, edges and corners); per-leaf masks: finer face, coarser face, any other level touching |
| Marching cubes + Transvoxel | `src/marching_cubes.cpp` | Lengyel's modified MC tables for regular cells; transition cells on faces toward a finer neighbor; Lengyel's shrink projected on the tangent plane |
| Transvoxel tables | `src/transvoxel_tables.inl` | Vendored from Lengyel's repository, MIT |
| Naive surface nets + skirts | `src/surface_nets.cpp` | One vertex per cell at the mean of its edge crossings; skirts along −normal |
| Test body | `tests/support/sp08_field.hpp` | A 300 m sphere, three octaves of `veach::hash` value noise (±7.9 m), a 7 m tunnel at 282 m height that opens on the surface about 100 m either side of the top. Also the seam check and the threaded meshing used by tests and bench |
| Unit tests `[sp08]` | `tests/voxel_tests.cpp` | Octree masks, watertightness, committed hashes on 1 and 4 threads |
| `veach-bench-sp08-mesher` | `tests/bench/sp08_mesher_bench.cpp` | Counts, ms per chunk, seam check, ray check, hashes, and a lab project (glTF + Luau cameras) for screenshots |

To reproduce (release build):

```sh
build/release/tests/veach-bench-sp08-mesher --threads 8 --repeat 3
build/release/tests/veach-bench-sp08-mesher --only tv,mc,sn,sn0 --no-highlight --out build/sp08 \
    --look 122.3,-37.1,90.8,128.0,-42.8,90.8
build/release/apps/lab/veach-lab --project build/sp08/tv --rhi metal --headless --frames 3 \
    --script scripts/look.luau --screenshot tv_look.png
```

Cameras written by `--out`: `overview`, `seam` and `seam_low` (the LOD0 | LOD1 face at z = +64 m), `corner` (the LOD0
square's corner), `rings` (LOD1 | LOD2 at z = +128 m), `cave` (the tunnel mouth), and `look` from `--look`. `--flat`
exports face normals; without `--no-highlight` transition cells and skirts are a second, white primitive.

## The Transvoxel license

The research marked it unverified. Checked on 2026-11-27:

- [github.com/EricLengyel/Transvoxel](https://github.com/EricLengyel/Transvoxel) holds `Transvoxel.cpp` (the tables) and
  a `LICENSE` file: **MIT License, Copyright (c) 2009 Eric Lengyel**, added by the author on 2023-11-02 (commit
  51a494f03c5b, the repository's latest).
- [transvoxel.org](https://transvoxel.org/) links that repository for the tables, says "The Transvoxel Algorithm is free
  of patent claims", and asks readers to cite Lengyel's 2010 dissertation. It states no other condition.

MIT is permissive (ADR 0023). The file is vendored as `engine/voxel/src/transvoxel_tables.inl` with the license text at
its top (only CRLF line endings changed), and listed in [THIRD_PARTY_NOTICES.md](../../../THIRD_PARTY_NOTICES.md).

## The setup

The octree has 512 m roots (2 × 2 × 2, so the body fits in ±512 m) and is refined around a focus at the top of the sphere.
A node at level l is split when the focus is closer to its box than λ × (the child's chunk edge); λ = 4 puts LOD0
within 64 m, as in ADR 0053. Result: 1,912 leaves (L0 608, L1 532, L2 436, L3 320, L4 16), 376 of them with surface,
304 with a finer face neighbor. Chunks whose box the bounds prove empty or solid are not sampled.

Samples are taken per chunk on a (32 + 3)³ grid (one apron sample each side, for central-difference gradients). A
Transvoxel face toward a finer neighbor also samples 3 layers of (64 + 3)² points at half the spacing, from the field
itself, so they are the neighbor's own boundary samples. Vertex positions are fixed point (LOD0 cells × 2^16) from an
integer expression both chunks evaluate identically; normals are gradients interpolated in float.

Numbers below are from an Apple M5 Max, one thread per chunk, best of 3 runs. Other spikes were building on the same
machine, so expect about ±30 % noise (plain marching cubes measured slower than Transvoxel in one run).

## Results

| | MC + Transvoxel | MC, no transitions (control) | Surface nets + skirts | Surface nets, no skirts |
|---|---|---|---|---|
| Triangles per meshed chunk (mean) | 2,099 | 2,081 | 2,300 | 2,107 |
| Vertices per meshed chunk (mean) | 1,121 | 1,108 | 1,251 | 1,153 |
| Transition or skirt triangles | 0.9 % (L1 1.3 %, L4 11.5 %) | 0 | 7.6 % | 0 |
| Mesh ms per chunk, median (p95) | **0.083 (0.118)** | 0.104 (0.146) | 0.377 (0.661) | 0.186 (0.226) |
| Sample ms per chunk, median | 2.46 | 2.5 to 3.2 | 2.7 | 2.6 |
| Total triangles | 789,402 | 782,386 | 860,222 | 783,831 |
| Open edges after welding by position | **0** | 7,016 (all at level changes) | 8,875 (skirt bottoms) | 7,029 |
| Non-manifold edges | 0 | 0 | 25,981 | 39 |
| Rays that see through rock (of 10.7 M) | **0** | 438 | 0 | 17,901 |
| Hash, 1 thread = again = 8 threads | `0x1b6a2a17bc27163c` | `0x2c01b0dffe9227c0` | `0x854b6594c1ed2cbb` | `0x8dda0e318c06f8b9` |

- **Seam check (exact).** All chunks' vertices are welded by their fixed-point position; an edge used by one triangle is
  a crack. Transvoxel has none, on all levels at once, with the default transition width (half a cell) and with width 0.
  The unit test checks the same on a smaller piece of the body that has LOD 0, 1 and 2.
- **Ray check (sampled).** 10.7 M parallel rays at 12.5 cm spacing over ±96 m around the focus, from 5 directions, each
  passing through the solid core so it must meet the surface. A ray whose first front face lies behind rock (checked
  against the field) saw through a crack. It misses slits narrower than the spacing; the seam check doesn't.
- **Determinism.** Every hash is equal across runs and thread counts. The unit test commits the hashes of the small
  piece (`0xbbc4053e4553b91d` Transvoxel, `0xec92ede1b61f4da6` surface nets), and Debug and Release agree on macOS
  arm64. Windows x64 is checked by CI.
- **All six faces.** Sampling and meshing every face of every level ≥ 1 chunk, as ADR 0053's per-frame toggle needs:
  mesh 0.100 ms (p95 0.132), sample **7.24 ms** per chunk.
- **Throughput.** Meshing is 3 % of a chunk's cost; the generator is the rest (35³ evaluations of this field at about
  57 ns each). The whole octree (1,912 leaves, 376 meshed) takes 250 ms on 8 threads. ADR 0053's B ≈ 300 chunks/s on a
  base M1 depends on the field-graph cost (SP-06), not on the mesher. Not measured on an M1.

## Screenshots

Lab, Metal, real-time renderer, constant magenta sky (so anything seen through the body is magenta), chunks colored by
level: L0 red, L1 orange, L2 green, L3 blue. `tv_` is Transvoxel, `mc_` plain marching cubes, `sn_` surface nets with
skirts, `sn0_` without.

| Image | What I saw |
|---|---|
| [tv_overview_highlight.png](sp-08/tv_overview_highlight.png) | The LOD0 square, the LOD1 and LOD2 rings and both tunnel mouths. Transition cells (white) form a continuous strip on the coarse side of every seam. No gap anywhere. |
| [tv_corner_highlight.png](sp-08/tv_corner_highlight.png) | The LOD0 square's corner from about 17 m: the two strips meet, with a small step where two coarse chunks meet. No gap. |
| [tv_cave.png](sp-08/tv_cave.png) | The tunnel's mouth at the LOD1 | LOD2 face: the roof is breached; the cave reads as a cave. |
| [mc_look.png](sp-08/mc_look.png), [mc_look_zoom.png](sp-08/mc_look_zoom.png) | The control, at the LOD1 | LOD2 face at x = +128 m from 8 m: a continuous dark crack along the seam (the renderer draws back faces, so the crack shows the body's inside, not the sky). |
| [tv_look.png](sp-08/tv_look.png), [tv_look_zoom.png](sp-08/tv_look_zoom.png) | Same view, Transvoxel: no crack. Two short dotted lines remain (3× zoom). They are not holes: rays cast on the CPU through those pixels all hit a triangle. They are transition triangles lying flat in the face plane at a chunk edge (one is 1.2 cm wide), where the rule below keeps vertices from moving; the renderer flips the normal of a triangle seen from behind, so they shade dark. |
| [sn_look.png](sp-08/sn_look.png), [sn0_look.png](sp-08/sn0_look.png) | Surface nets: without skirts the seam is a step; with skirts no rock shows through, but the coarse and fine strips overlap and the skirts show as dark slivers. |
| [sn_corner_flat.png](sp-08/sn_corner_flat.png) | Surface nets with skirts at the LOD0 corner, flat shaded: the overlap boundary is ragged along every seam. |

Two artifacts I chased and that are not the meshers':

- **Pinholes with two glTF primitives per chunk.** Exporting transition triangles as a second primitive (to color them)
  gave sky-colored pinholes along some shared edges (8 to 34 pixels per image). The same triangles as one primitive per
  chunk give none. Positions are bit-identical either way, so the suspect is the importer or the visibility path for
  meshes with several primitives. The screenshots without `_highlight` use one primitive per chunk.
- **Float rebasing.** Chunk-relative positions are multiples of 2^-17 m; a float holds them exactly while the chunk is at
  most 2^24 × 2^-17 m = 128 m, so up to level 3. At level 4 the two chunks round a seam vertex differently (36 cases
  here). Not visible in these images; it would be at far seams on a larger body.

## What Transvoxel needed beyond the paper

- **A vertex on a face plane whose neighbor is not finer must not move.** Lengyel's shrink moves every regular vertex in
  the boundary cell layer. Next to a transition corner that includes vertices on the plane shared with a same-level
  neighbor, which doesn't move them: a crack at a same-level seam. With the rule the weld check is clean; the cost is the
  flat transition triangles at those edges (above).
- **Identical samples at shared lattice points** (finding 1 in the verdict). The field here has no band limit, so it
  holds trivially.
- **Winding.** Lengyel's tables, with case bit i set for a negative (solid) corner, give counter-clockwise triangles seen
  from outside the solid: no flip for glTF. Transition triangles flip on maximum faces and when the class's high bit is
  set. 99.97 % of triangles agree with the gradient; the rest are folds at thin features and shrunk cells.

## What surface nets needed, and why it loses

Surface nets has no seam scheme of its own. Skirts alone were not enough: each chunk's mesh stops half a cell short of
its maximum faces, so from above there was a gap between the two skirts (1,587 see-through rays with 2-cell skirts). It took two more
rules: on faces toward another level the chunk also emits the quads on its maximum face (the two levels overlap), and
faces that touch another level only through an edge or a corner get skirts too. Then no rock shows through, but:

- it is not watertight (skirt bottoms are open, skirts at diagonal contacts are non-manifold), so it can't feed
  colliders as is, and it can't be checked exactly the way Transvoxel can;
- the overlap strip is ragged and the skirts show (screenshots above);
- the research expected about half the vertices of marching cubes; measured, it has 4 % more (1,153 against 1,108), and
  the same triangle count within 1 %;
- it meshes 2.2× slower as written (4.5× with skirts); that is unoptimized, but it is not faster.

ADR 0053 reopens if "surface nets wins clearly". It doesn't.

## Decisions for the dev lead

1. **Band limit (ADR 0052/0053).** Recommendation: the generator's band limit at a sample is a function of the sample's
   position only (for example, of the finest level allowed there), never of the level asking, so a coarse chunk's corner
   equals the fine neighbor's sample. The alternative, meshing coarse faces from the fine chunk's samples, breaks chunk
   independence.
2. **Six faces always, toggled in the shader (ADR 0053)?** It costs 3× the sampling per chunk as built. Options: keep it
   and sample faces in one layer (normals from the fine neighbor's gradient formula need the other two), or mesh only
   the masked faces and remesh a chunk when a neighbor's level changes (meshing is 0.08 ms; sampling is what costs).
   Recommendation: remesh on neighbor change, keep the samples of chunks near the camera so a remesh doesn't resample.
3. **Vertex position format.** Recommendation: snap chunk-relative positions to a per-level grid that floats hold
   exactly (2^-17 m × 2^max(0, l − 3)), or ship them as integers and convert in the vertex shader.

## Follow-ups

- Find the multi-primitive pinhole bug in the importer or the visibility path (a backlog item).
- Shrink rule at chunk edges that moves the flat transition triangles off the face plane without breaking same-level
  seams (both chunks must apply the same rule from the same masks), or shade terrain without the back-face normal flip.
- Secondary positions per vertex and per-face transition ranges, if decision 2 keeps the shader toggle.
- Measure on a base M1 with the SP-06 field graph instead of this hand-written field, against B ≈ 300 chunks/s.
- The real `engine/voxel` replaces this code: chunk store, edits, `ChunkProducer`, job system (VCH-015), runtime meshes
  (VCH-102), colliders from LOD0 meshes.
