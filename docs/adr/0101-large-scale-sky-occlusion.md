# 0101: Large-scale sky occlusion from a height map of the scene drawn from above, traced per 8 x 8 pixel cell

Date: 2026-11-28

## Status

Accepted. Builds on [0096](0096-ambient-light-and-occlusion.md) (the ambient table, GTAO, `ambient_light`) and
supersedes, within its reach, the camera-wide `SkyVisibility` of [0092](0092-aerial-perspective-follows-the-air.md)
decision 3 for the ambient light. Built as part of VCH-141 ([spec](../specs/sky-occlusion/spec.md),
[measurements and screenshots](../research/sky-occlusion/README.md)).

## Context

The ambient table is the open sky over open, sunlit ground; GTAO reaches 1.5 m in screen space; `SkyVisibility` is one
number for the camera's surroundings. At sunset Bistro's facade sat in the opposite buildings' shadow but kept the
bright sky around the sun they hide; walls in a street got the open field's ground bounce; caves and the dug hole were
lit by sunlit ground below. No ray tracing (0039). Options weighed: (a) a scene representation around the camera
traced per pixel; (b) screen space plus a terrain horizon; (c) the sun's shadow cascades sampled in sky directions;
(d) irradiance probes with visibility.

## Decision

1. **A height map of the scene from above, around the camera.** Three depth-only views from straight above (the
   camera's up), drawn through the GPU-driven culling with the shadow cascades' rules (no normal cones; casters under
   half a texel left out): the highest surface of each texel, the lowest, and the highest face turned down (one
   culling, three draws, `shadow_vs` and the casters' coverage test). 1024² over 2 × 250 m (half a meter a texel at the
   default reach of 200 m), reverse-Z, pancaked like the cascades. A compute pass turns them into heights: the top, the
   solid column from below (the lowest surface; the top when a face turned down sits at the lowest surface, a closed
   object), and the bottom of the slab under the top (the highest face turned down above the lowest surface, a
   ceiling; else a quarter of the gap, 0.5 to 4 m: eaves, awnings, canopies are thin). A pyramid of 8 mips keeps the
   highest top and column and the lowest slab bottom. The map is drawn again only when its center, snapped to the
   coarsest mip's texels (62.5 m) in the render frame, moves, or every 60 frames for what moves in it; so a still
   camera's image never depends on where the camera came from.
2. **Traced per 8 x 8 pixel cell.** One thread per cell (pixel 8i + 4): its point from the visibility depth, its
   triangle's normal from the visibility buffer, moved off the surface by 1.5 texels. In 8 fixed azimuths, 12 steps
   from 1 m growing by 1.6 (to about 180 m), each read from the mip as long as the step; a column hides the elevations
   up to its top seen from the step's near end, a slab from its bottom seen from the far end to its top seen from the
   near end, on a 32-bin mask of 0 to 90 degrees, which becomes one arc of the same size and centroid. A slab over the
   point's own texel hides the zenith. Three points in the air in front of what each azimuth sees (the occluder, the
   ground 20 and 50 degrees down) are looked up in each shadowed light's cascade (0095); a sun without a shadow map
   walks the height map toward it.
3. **The ambient table with and without what is hidden.** Per sun and per azimuth's sector, the table's radiance read
   toward the middles of the open sky in the normal's hemisphere, of the visible sky, and of the ground, in halves,
   with a lobe as wide as a sector, by the same quadrature with and without the occlusion. In place of the hidden sky,
   the surroundings' bounce: the table's sunlit ground where what hides it is in the sun, half an upward face's
   sky light times the ground's albedo where it is in shade; the ground the same way by its own sunlit share. The
   cell stores the ratio (rgb diffuse, a specular, the specular lobe's hidden share by elevation in its azimuth) and
   its distance; a 3 × 3 blur that never crosses a 5 % change of distance; the shading reads the 4 nearest cells
   weighted by distance (`sky_occlusion_at`).
4. **One seam.** `AmbientSurface::sky` carries the cell ratios into `ambient_light`, which scales the table's diffuse
   and specular light by them before GTAO and lets the camera's `SkyVisibility` apply only where the large-scale
   occlusion fades out (the last quarter of `AmbientOcclusion::sky_distance`, default 200 m; 0 turns it off). The shade
   pass gets it through one handle (`ShadeParams::sky_occlusion`, a `SkyOcclusionFrame`) and one flag.

## Consequences

- Bistro's facade at sunset reads as shadowed and close to photo mode; at noon the street's walls lose the sky the
  opposite buildings hide; under the awnings it is dark; the SP-14 cave and the slice's dug hole are lit by what the
  opening shows, not by sunlit ground; the open field is unchanged. The Bistro goldens (FLIP 0.18 at noon, 0.58 at
  sunset) and the terrain materials golden (0.045, its cave and hole) were regenerated; no other golden moved.
- Cost on the M5 Max, median of two rounds in Bistro: trace and blur 0.16 to 0.17 ms at 1080p and 0.26 to 0.27 ms at
  1440p; the GPU frame span +0.14 to +0.19 ms and +0.24 to +0.35 ms. Drawing the map takes 1.7 to 2.3 ms of passes in
  Bistro (+1.4 ms of frame span) on the frames that draw it: when the camera crosses a 62.5 m cell or every 60
  frames. 12 MB of transient depth maps on those frames; 11 MB of heights and their mips kept (rgba16_float).
- Limits of a height map: air under a slab that overlaps a solid below it in the same texel reads as solid (two boxes
  overlapping); several floors are one; a 0.5 m texel steps the transition at a cave's drip line and in a 2 m hole
  (blocky dark patches at the bottom of the s = 1 hole); a single sample per step can miss a wall thinner than the
  step's mip texel (faint streaks on a chamber's floor). The sunlit bounce assumes the surroundings' albedo is the
  ground's.
- The height map is a second set of passes beside the cascades with the same shape: the culling and drawing could be
  shared with the shadow renderer (and with 0100's caching) — VCH-141.
- Revisit if: interiors with several floors matter (a coarse voxel occupancy or SDF around the camera, option (a)'s
  other form), the redraw frames' spike matters (stagger the three layers over three frames, or cache static layers as
  0100 does for far cascades), or M1-class measurements (VCH-097) put the trace over budget (fewer steps or azimuths).

## Alternatives considered

- **Shadow cascades sampled in sky directions (c):** a cascade is a height map seen from the sun; at sunset, the case
  that matters, it is a side view, and every ray behind a building reads as blocked. Used instead for what it answers
  exactly: whether the surroundings are sunlit.
- **Screen space plus a terrain horizon (b):** misses what is off screen (the buildings behind the camera, a cave's
  ceiling out of view) and needs coarse terrain heights the engine does not have (0042).
- **Irradiance probes with visibility (d):** a probe grid's visibility needs a scene representation anyway (rays or
  depth cubes per probe), and leaks at walls without one; much more to build for the same signal.
- **A voxel occupancy or distance field around the camera:** handles overhangs and floors; a 3D texture, a
  voxelization pass and cone tracing, several times the cost and memory for a scene that is mostly 2.5D here.
- **Per-pixel or quarter-resolution tracing:** 0.63 ms at 1440p at quarter resolution, noisy per pixel; the cell
  ratio is smooth by construction (one per 8 × 8, blurred), and the shading adds the normal's detail through the table.
- **Subtracting the hidden sky from the table's irradiance:** the quadrature's error left blue light in fully hidden
  places; the ratio of one quadrature with and without the occlusion is 1 in the open and 0 when all is hidden.
- **A bent normal and a cone:** one cone cannot hold a street's two sides and the slot of sky between them.
- **A coarser mip per step (catching every wall):** its maximum takes in the surface's own building; facades went black.
