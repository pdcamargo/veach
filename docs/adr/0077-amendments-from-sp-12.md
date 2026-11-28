# 0077: Amendments from SP-12: brush reach, owned override samples, K = 2, waking bodies on collider swaps

Date: 2026-11-27

## Status

Accepted. Amends [0053](0053-terrain-density-field.md), [0054](0054-terrain-edits-and-water.md) and
[0058](0058-physics-jolt.md). Validated by SP-12 (VCH-088, [findings](../research/spikes/sp-12-dig-collide.md)).

## Context

SP-12 dug into an Earth-radius planet 6,371 km from its center: a six-op edit log applied in fixed-point integer
arithmetic on `veach::JobSystem`, chunks meshed with SP-08's mesher, Jolt `MeshShape` colliders cooked from them and
installed in chunk-key order, and a ball that falls into the pit and later into a sump dug under it while it sleeps.
Edited samples, meshes, cooked shapes, the install and the ball's state after every step hash the same on arm64 and
x86-64 (SSE2 and default ISA), with 1 and 4 workers and with ops applied to chunks in reverse order; the dug terrain
has no holes across 98,584 triangles. Per chunk of about 2,088 triangles on the M5 Max: sampling 1.75 ms, meshing
0.05 ms, cooking 0.46 to 0.65 ms. One brush per 60 Hz tick on 4 workers goes from op to installed colliders in 0.84 ms
(1.5 m radius), 1.10 ms (4 m) and 4.5 ms (16 m, the cap). SDF colliders, the pivot, are not needed. The brush op and
its integer applier now live in `engine/voxel` (`edit.hpp`), where 0054 puts them.

## Decision

1. **A brush rewrites samples only within its radius plus 2 LOD0 cells**, and that reach is part of each brush kind's
   canonical definition. A subtract that rewrote every sample its falloff could reach would touch everything within
   32 m.
2. **Override records store only the samples a chunk owns** (0 to 31 on each axis), so each point is stored once; a
   chunk's border samples come from its neighbors when it loads. This replaces 0054's "apron included" at rest; the
   apron is still written in memory while an op applies.
3. **An edit samples a chunk the bounds proved empty or solid** (0053) the first time it touches it, about 1.8 ms once
   per chunk.
4. **K = 2:** a collider requested at tick t is installed before step t + 2. A cap-size op takes 5 to 10 ms on the
   M5 Max, and K = 2 leaves 33 ms for slower machines than the design target; K = 1 would require every op cooked within
   the tick it was committed in, on every machine.
5. **Swapping a chunk's collider wakes the bodies over it**, and the shape is replaced in place so body ids stay
   stable. Jolt does not wake them itself: without the wake, a sleeping ball hovered over a freshly dug sump.
6. **A surface physics scene's origin sits on a LOD0 grid point, with the planet's axes**, so collider vertices equal
   the canonical mesh's lattice positions (0074) exactly, with no rounding at the scene boundary.

## Consequences

- Digging is cheap enough to run every tick: a hand-sized brush costs 5 to 7 % of a tick on 4 workers, a cap-size one
  27 %. Streaming 89 colliders costs 12.8 ms on 4 workers.
- Overrides from hand-sized digging stay smaller than the op log that made them (4 KB against 11.5 KB); thirty
  cap-size ops leave 378 KB against 1.4 KB of ops. That matters for the brush cap, which is still a working default
  pending the owner's review.
- Physics stalls rather than diverges if a machine can't cook within 33 ms, as 0058 intends.

## Alternatives considered

- **K = 1:** tighter feedback, but a cap-size op on a slower machine misses it and stalls the step.
- **Overrides with aprons at rest:** simpler loading, but each border point is stored in up to eight chunks and the
  copies can disagree after a partial save.
- **SDF colliders:** the pivot if cooking couldn't keep up; it can, and mesh colliders match what is drawn.
