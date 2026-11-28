# 0086: Amendments from building engine/voxel: provable box bounds, whole-selection swaps, a thin binding in runtime

Date: 2026-11-28

## Status

Accepted. Amends [0053](0053-terrain-density-field.md), [0054](0054-terrain-edits-and-water.md),
[0065](0065-engine-areas-for-the-toolbox.md) (the voxel row) and clarifies
[0074](0074-amendments-from-sp-08.md) rule 2. Found while building `engine/voxel` (VCH-114,
[spec](../specs/voxel-terrain/spec.md), [guide](../guides/voxel-terrain.md)).

## Context

`engine/voxel` now streams a terrain body from a procgen field program: a restricted octree around any number of
viewers, chunk jobs across frames under a `TerrainStreamingBudget`, an edit store with owned override records, and
LOD0 collider meshes with adjacency rings. SP-12's committed base and edited sample and mesh hashes reproduce through
the edit store, and the lab's descent from orbit to 10 m draws 0 open edges at every checkpoint. SP-09's 100 m
benchmark runs at 213 / 824 / 2,786 chunks/s on 1 / 4 / 17 threads of the M5 Max with a 19-octave procgen planet,
against SP-09's 129 / 457 / 1,678 with its 21-octave C++ generator. Building it showed where 0053, 0054 and 0065 say
more than the code needs, or something different.

## Decision

1. **Box bounds are proven from the field program, not cached.** 0053's "cached min/max pyramid" is replaced by
   `procgen::lattice_bounds` and `procgen::lattice_variation`: per output a value range, a Lipschitz constant and the
   most it changes within a radius (each octave counts the smaller of its slope times the radius and its range), from
   the op list alone. A chunk's box is classified with one evaluation at its center, split in eight up to 6 times
   (SP-09's depth), and the class is cached per node. Bounds are canonical, so every machine skips the same chunks.
2. **"Never a hole" swaps whole selections.** The drawn set changes only when every chunk of the new selection is
   meshed, SP-09's policy, with the effective λ lowered after 15 lagging updates (×0.97 per update, at least 2) and
   recovered at ×1.005. 0053's per-parent rule ("a parent stays drawn until its children are meshed") would need
   meshes for transition masks of mixed old and new neighborhoods; a per-subtree policy waits until measurements on
   the M3 Pro (SP-15) show the whole-selection swap lags too long.
3. **Jobs are scheduled coarse levels first, then by distance to the nearest viewer**, not by screen-space error:
   under the whole-selection swap only the order across frames matters, and coarse chunks cover the most screen.
   Hysteresis (merge beyond λ·1.1 child edges) keeps a hovering viewer from flipping nodes every frame.
4. **Transition cells go on the faces whose neighbor is finer**, in the coarser chunk (Transvoxel's convention). 0074
   rule 2's "faces that border a coarser neighbor" describes the same seam from the finer side; the mesh of a chunk
   depends on its finer-face mask, and a change of that mask remeshes it.
5. **Coarse levels read edits at their own lattice points.** 0054's "propagated up" overrides are not stored per
   level: a chunk of any level overlays the LOD0 records at the lattice points it samples. The result is 0054's exact
   derivation (a coarse sample is the LOD0 sample at the same point) with one record per edited LOD0 chunk.
6. **LOD0 samples are not kept around players.** 0053's 125 resident LOD0 chunks are replaced by the edit store
   (records for edited chunks, sampled on first edit, 0077 rule 3) and the collider meshes (kept for the chunks
   within reach and their neighbors). Unedited samples are regenerated when needed; remeshes re-sample (0079 rule 4's
   cached samples remain the next lever).
7. **The voxel area depends on core and procgen only.** 0065 lists `ecs`, `asset` and `physics` too. The ECS-facing
   binding (`TerrainModule`: a body per reference frame, `TerrainViewer` entities, upload to the `MeshStore`,
   extraction through `RenderSpaceMap`) lives in `engine/runtime`, which already depends on everything; physics gets
   collider meshes through `voxel::ColliderSink`, data only (chunk key, lattice positions, own triangles, ring
   triangles, install tick). The adapter that cooks and installs them belongs with VCH-113's API, in
   `engine/physics` or in the runtime binding, so neither area links the other. `check_repo.py` now also fails if
   `engine/voxel` includes or links Jolt.
8. **`TerrainBody` is generic, in `engine/voxel`.** 0053 put it in `space/terrain`. A body is a density source (a
   sphere, a plane or nothing, plus a field program) on a body-local lattice in its reference frame; nothing in it is
   planetary. `space/` keeps the planetary graphs, scale factors, radial up for brushes and sea level, and creates the
   bodies (VCH-115).
9. **Chunk vertices are chunk-relative floats; chunk origins go to render space in double.** Vertices are converted
   from lattice coordinates on the chunk's job (exact up to level 3), and each chunk's origin goes from lattice
   integers to the frame's meters and through `RenderSpaceMap` relative to the render origin in double at extraction.
   Coarse chunks' shared vertices can differ by a float ulp of the chunk size (1.5 cm at 262 km chunks, seen from
   hundreds of kilometers): below a pixel, and the canonical weld check on lattice positions stays at 0 open edges.

## Consequences

- A flat voxel game uses `engine/voxel` and `TerrainModule` without `space/`; a server streams colliders with no
  renderer (the binding skips the upload when there is no `MeshStore`).
- Bounds are looser than SP-09's hand-derived ones: at the 100 m benchmark 23 % of selected chunks have no
  triangles (SP-09: 8 %), and the selection holds 2,438 chunks (SP-09: 1,591). A tighter proven noise bound, or a
  cached per-level min/max of the planetary band, is the lever if streaming falls behind on the design target.
- A selection that keeps changing under sustained overload lags until λ drops: in the lab at s = 0.1 and 32 jobs
  per frame, the 100 m skim lowered λ to 2.1 and the drawn set lagged up to 48 frames, with no hole.
- Physics integration needs one adapter from `ColliderUpdate` to VCH-113's install API.

## Alternatives considered

- **A cached min/max pyramid per body (0053):** tighter for heightfields, but a second representation to keep in
  sync with edits and graph versions; the proven bounds need no cache and also cover 3D caves.
- **Per-parent swaps:** fewer lagging frames under overload, at the cost of meshing extra transition masks; deferred
  until measured on the design target.
- **The binding inside `engine/voxel` with ECS and render dependencies:** fewer files, but voxel would link render,
  which 0065 forbids.
