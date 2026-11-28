# 0083: Terrain colliders carry their neighbors' border triangles as adjacency

Date: 2026-11-27

## Status

Accepted. Amends the terrain colliders of [0058](0058-physics-jolt.md) and [0077](0077-amendments-from-sp-12.md).
Found by the seams sweep (VCH-116, [findings](../research/spikes/seams-sweep.md)).

## Context

The seams sweep walked characters, dug pits and dropped balls at the octree's octant planes, both poles, cube-corner
directions and a point where eight chunks meet: zero open edges, no fall-through, no contact loss over 1.07 M steps,
up radial to 1.2e-7 rad and a heading continuous over the poles. Negative coordinates are correct throughout. Nothing
the terrain reads is cube-face sampled. It found one problem, at every chunk border rather than at those places:
Jolt treats each chunk collider's border edges as sharp, because the triangles past them belong to another body. A
walking character gets a one-step vertical bump of up to 1.2 m/s there; near borders the p99 per-step |Δv| is 0.6 to
1.3 m/s with per-chunk colliders against 0.2 to 1.2 m/s with the same triangles merged into one collider. This very
likely explains SP-13's 13 to 14 contact losses per day at chunk seams.

## Decision

Each chunk's collider is cooked with a **ring of its neighbors' border triangles as adjacency only**: they inform
Jolt's internal-edge handling and never produce contacts. Measured against the merged control on every walk, it
matches within 0.003 m/s at p99, for 9 to 11 % more triangles per collider. A neighbor's remesh recooks the chunks
whose ring it feeds, in the same t + 2 install, in chunk-key order. Built in `engine/physics` and `engine/voxel`
(VCH-113, VCH-114); rigid bodies other than characters are measured there before it is final for them.

## Consequences

- Characters and rolling bodies cross chunk borders as smoothly as a single mesh.
- An edit near a border recooks up to the neighbors that share its ring; cooking is about 0.5 ms per chunk (0077).
- The cube-corner directions need no further testing for this octree.

## Alternatives considered

- **One merged collider per region:** smooth, but every edit recooks the whole region.
- **Jolt's enhanced internal edge removal, or plainly duplicated border rings:** measured; neither removes the bump.
