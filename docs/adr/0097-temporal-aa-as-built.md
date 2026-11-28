# 0097: Temporal AA as built: unjittered views, surface ids for disocclusion, the backdrop after the resolve

Date: 2026-11-28

## Status

Accepted. Amends [0061](0061-temporal-accumulation-no-upscaling.md) (temporal accumulation at native resolution).
Implemented by VCH-144 ([spec](../specs/temporal-aa/spec.md), [findings](../research/spikes/vch-144-taa.md)). Closes
SP-16's moved kill criterion ([SP-16](../research/spikes/sp-16-visibility-vs-forward.md)): ADR 0060 stands.

## Context

ADR 0061 decided TAA in `post_hdr` on pre-exposed radiance, Halton (2, 3) jitter of length 8, a YCoCg neighborhood
clamp and luminance weighting, invalidation on cuts. Building it on the GPU-driven core ([0093](0093-gpu-driven-core.md))
left open where the jitter lives, how disocclusion is found in dense foliage, how stars drawn per pixel
([0091](0091-space-backdrop.md)) survive a temporal filter, how goldens stay deterministic, and what it costs. SP-16
asked whether ghosting on thin alpha-tested foliage is acceptable by eye, with forward plus 4x MSAA and alpha to
coverage as the fallback.

## Decision

1. **The views stay unjittered; the real-time renderer jitters itself.** `RenderView` carries `temporal_aa` and
   `jitter` (pixels, from `render::taa_jitter(frame)`), and its matrices are never offset: photo mode (which hashes the
   view to restart accumulation), overlays drawn after the display pass and every other feature see the camera as it
   is. The real-time renderer builds `jittered_view_projection` for culling, the visibility pass and shading (whose
   rays must hit the triangles the raster saw), keeps last frame's jittered matrix for the HiZ test and the unjittered
   one for motion. Motion vectors subtract this frame's jitter in the visibility pass; the sky's come from the
   unjittered rays. A feature that reads the depth buffer reconstructs positions with the jittered matrix.
2. **Disocclusion by surface id and depth, over the 3x3 history texels.** Each `InstanceRecord` carries a 24-bit id
   hashed from its drawing entity (terrain chunks: their key's id), stable across frames. The history keeps each
   pixel's id, its sample count and linear depth (`rg32_uint`). A pixel keeps its history if any of the 3x3 history
   texels around where it was shows its surface: the same id (moving objects keep their history whatever their
   depth), or another at the depth last frame's camera would have seen this one at (neighbors, a new LOD of the same
   ground). Jitter moves every edge by up to a pixel between frames, so the surface an edge pixel shows alternates;
   testing only the texel it lands on reset foliage edges every frame. Found only beside where it was while moving,
   this frame weighs at least `taa_edge_weight` (the trail case clipping misses: a foliage edge's neighborhood holds
   both colors). Teleports and cuts within one camera need no detection: nothing matches, everything starts over.
3. **The blend.** Variance clipping (1 standard deviation, inside the min/max box) in YCoCg of Karis's compressed
   color c / (1 + luma); this frame weighs 1 / samples while the history is young (a fresh pixel converges as an
   average), at least 1/12 still and 0.5 for a pixel that moves a pixel a frame; a 5-tap Catmull-Rom history fetch;
   then a limited unsharp mask on the displayed image scaled by motion (resampling softens moving history; a still
   image is not sharpened). Tuned on SP-16's field against 16-sample references, still and moving.
4. **The space backdrop after the resolve.** The shade pass marks the sky behind which the backdrop shows (alpha 2,
   geometry 1); the resolve blends and clips alpha like the color, so it becomes 1 + the share of the pixel that shows
   that sky; the backdrop composite then adds stars, band and nebulae times that share, along unjittered rays. Stars
   are never filtered in time (deep space is bit-identical with TAA on and off) and silhouettes against the stars are
   still anti-aliased. Metering and the display pass read the result; auto-exposure's NaN check still sees NaN.
5. **Exposure.** The history is stored at its frame's pre-exposure and rescaled by state[0].y / state[0].z under
   auto-exposure (the resolve runs before this frame's metering), or by the ratio of camera exposures without it.
6. **Invalidation** as 0061, implemented as: another camera, render frame, target size or exposure control, or a frame
   in which the resolve did not run (photo mode in between). The render origin's 64 m jump is not a cut: last frame's
   matrices and transforms are moved into this frame's render space (0093), and the history is screen space.
7. **Goldens.** `golden_test.cmake` sets `VEACH_DISABLE_TAA=1` unless a test says `TAA`: every existing golden tests
   one frame's shading and stayed byte-identical; temporal goldens render a fixed frame count of the fixed sequence and
   are byte-identical from run to run (`taa_motion`). The origin jump is tested by comparing two runs by FLIP.
8. **Per camera:** `AntiAliasing { temporal = true }` (no component: on). No sharpness or weight settings for creators
   yet.
9. **SP-16's verdict: keep ADR 0060; the MSAA fallback is not needed.** On the field TAA lowers FLIP against the
   16-sample reference in every still view (ground 0.036 → 0.033, mid 0.065 → 0.056, far 0.125 → 0.101; SP-16's 4x
   MSAA managed 10 to 17 % on its own renderer), removes the shimmer no-AA adds in a slow pan (far: no-AA's
   frame-to-frame change 2.5 times the reference's, TAA's below it), and shows no ghost trails by eye strafing
   through the grass at 6 and 18 m/s or panning. What it costs: the image is softer than the reference while the
   camera moves (on the ground view FLIP in motion is 9 to 27 % above no-AA's, which is sharp but crawls; on the mid
   and far views it stays below), and a faint one-pixel rim can follow a fast silhouette. The Metal-only MSAA stays in the RHI, documented as spike-only, until it is clear no
   preset needs it.

## Consequences

- Cost on the M5 Max (render-graph timestamps, SP-16 field, release): the resolve 0.33 to 0.37 ms and the sharpening
  0.07 to 0.08 ms at 1080p; 0.50 to 0.54 ms and 0.12 ms at 1440p; Bistro at 1440p 0.42 + 0.12 ms. 0061 estimated 0.08
  ms; the research's 0.3 to 0.5 ms was closer. Groupshared neighborhoods and reading the history ids only where the
  landing texel misses took 0.44 to 0.33 ms; dropping the instance-id fetch, a history write or four of the five
  history taps changed nothing measurable, so the pass is bound by its memory traffic as a whole. Folding the
  sharpening into the display pass is the next saving (VCH-145).
- Every geometry feature writes motion and an id: a draw without an entity gets id 0 and shares it.
- Memory: two `rgba16_float` and two `rg32_uint` history targets at output size, 66 MB at 1080p and 118 MB at 1440p,
  plus two transient `rgba16_float` targets (halving the ids is in VCH-145).
- Revisit if: a preset cannot afford 0.4 ms (fold the sharpening, halve the ids), transparency or particles arrive
  (they need a reactive mask or to be drawn after the resolve), or creators need softness control.

## Alternatives considered

- **Jittered `RenderView` matrices** (every feature sees the jitter): photo mode would restart every frame and
  overlays would shake; each would need to opt out.
- **Depth-only disocclusion:** a surface moving along the view fails the static-depth test and loses its history.
- **Reprojected visibility-buffer instance indices:** positional in the GPU scene, they change when entities come and
  go.
- **The backdrop through TAA with unjittered rays:** stars stay put while still, but smear when the camera turns, and
  variance clipping makes sub-pixel stars flicker; drawing them after costs nothing.
- **Goldens regenerated with TAA on:** every real-time golden would change, and the shadow and AO work in parallel
  regenerates the same images; a TAA regression would hide in shading diffs.
- **Sharpening everywhere, or none:** a still image is already as sharp as the 16-sample reference allows; in motion
  the motion-scaled sharpening lowered FLIP against the moving reference (ground view, slow pan, 0.048 to 0.046 at the
  same weights).
