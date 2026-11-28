# 0074: Amendments from SP-08: LOD-independent samples, remeshing on neighbor change, lattice positions

Date: 2026-11-27

## Status

Accepted. Amends [0053](0053-terrain-density-field.md). Validated by SP-08 (VCH-084,
[findings](../research/spikes/sp-08-mesher.md)).

## Context

SP-08 meshed a cave-carved sphere in 32³-cell chunks across touching LOD levels with three meshers in a small
`engine/voxel`. Marching cubes with Transvoxel transition cells (0.083 ms per chunk, 2,099 triangles) left zero open
edges and zero rays through rock across levels 0 to 4; plain marching cubes left 7,016 open edges; surface nets with
skirts was slower and not watertight. ADR 0053's mesher choice holds, and meshing is about 3 % of a chunk's cost next
to sampling the density (about 2.5 ms). The tables are Eric Lengyel's, MIT since 2023, vendored and listed in
`THIRD_PARTY_NOTICES.md`. SP-08 also found three conditions the seams depend on that ADR 0053 does not state.

## Decision

1. **A density sample is a pure function of its position.** The level of the chunk asking for a sample never changes
   the value; any band limiting depends on position only. Transvoxel closes a seam only if both sides read the same
   sample at the shared points.
2. **A chunk is remeshed when a neighbor's level changes**, with transition cells only on the faces that border a
   coarser neighbor. Building all six transition faces up front and toggling them in the shader would triple sampling
   per chunk (2.5 to 7.2 ms).
3. **The mesher emits positions in integer LOD0-lattice coordinates plus a quantized fraction along the crossing
   edge**, not chunk-relative floats. Chunk-relative float32 positions are exact only up to level 3 (128 m chunks);
   from level 4 the same seam vertex rounded differently in the two chunks. Conversion to float happens relative to
   the render origin at extraction ([0045](0045-large-world-positions-and-reference-frames.md)), so a shared vertex is
   bit-identical on both sides of a seam at any level.

## Consequences

- Seams stay closed at every LOD depth the 21 levels of an Earth-sized body need.
- Field graphs must not band-limit by the caller's LOD; the procgen runtime enforces it by giving nodes positions,
  never a level ([0052](0052-procgen-runtime.md)).
- A level change costs a remesh of the neighbors, at 0.08 ms each: small next to sampling.
- The real mesher's vertex format changes from SP-08's prototype.

## Alternatives considered

- **Surface nets:** about the same vertex count as marching cubes here, not half, and its seams need skirts that show
  as dark slivers.
- **All six transition faces per chunk:** triple the sampling cost for avoiding remeshes that cost little.
- **Double-precision chunk-relative vertices:** fixes the rounding but doubles vertex size, where lattice coordinates
  are exact and compact.
