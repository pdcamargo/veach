# 0095: Sun shadows: cascades culled and drawn by the GPU-driven path, chosen by view depth, filtered by the sun's size

Date: 2026-11-28

## Status

Accepted. Implements the cascaded half of [0062](0062-shadows-ambient-reflections.md)'s shadows in the renderer of
[0093](0093-gpu-driven-core.md).

## Context

ADR 0062 decided cascaded shadow maps for at most the two brightest suns, analytic occluders for anything planet-sized,
stable texel-snapped cascades whose split distances are meters at every scale, and PCF through comparison samplers.
It left open how cascades meet the GPU-driven core (0093: compacted indirect lists, no count buffers, two-sided
rasterization), where they end and what lies beyond, how they are filtered and biased, and where the lookup sits in
the shading that other features (GTAO, TAA, sky ambient) change at the same time. The RHI has depth bias, comparison
samplers and 2D arrays (0090) but no depth clamp. Render space moves on a 64 m grid (0045, 0085). Spec:
[sun shadows](../specs/sun-shadows/spec.md); backlog VCH-142; measurements:
[sun shadows](../research/sun-shadows/README.md).

## Decision

1. **Which lights.** The two brightest directional lights by luminance (`render::shadowed_lights`), fewer if the
   camera's `SunShadows.suns` says so. Eclipses stay in the light's radiance, as before (0063); other lights are
   unshadowed.
2. **Cascades.** Up to four per light (default four, 2048² each, in one `depth32_float` 2D array, a layer per
   cascade), the last ending at `SunShadows.distance` (default 1000 m), each a quarter of the next (15.6, 62.5, 250 and
   1000 m). Each is the bounding sphere of its slice of the camera's frustum (its size never changes as the camera
   turns), in a light frame built from the light's direction only, centered on a whole texel of a grid fixed in the
   render frame: the snap is computed in double from render space's origin, so crossing a 64 m cell moves no shadow
   (`render::fit_cascade`, `veach/render/shadow_cascades.hpp`). Orthographic, reverse-Z, depth 1 at twice the radius
   toward the light; casters beyond are flattened onto the near plane in `shadow_vs` ("pancaking"), so no RHI depth
   clamp is needed.
3. **Culling and drawing go through the GPU-driven path.** The same kernels (`gpu_cull.slang`) with
   `cull_shadow_view`: the cascade's box instead of a frustum, no HiZ, no normal cones, and two more tests: a caster
   must be at least 0.35 texel in radius, and its bounding sphere swept away from the light must not lie wholly outside
   any plane of the camera's slice that cascade serves (conservative: a plane the sweep heads into never culls).
   Each cascade has its own counters, draw arguments and compacted lists in the camera's layout; all cascades' culling
   runs in four compute passes (a dispatch per cascade); each cascade's casters are one depth-only render pass of the
   eight indirect draws, opaque with an empty fragment shader, alpha-tested with the visibility pass's coverage test,
   rasterized two-sided with a slope-scaled bias (-1.5). One set of maps serves every frame in flight.
4. **The lookup is one call in the shading function**: `light_shadow` (`veach/shadows.slang`), reached through
   `ShadeParams::shadows` (a `ShadowFrame` in upload memory), multiplies each light's irradiance and returns 1 for a
   light without a map. The cascade is chosen by view depth, the same slices culling used; the last tenth of each
   cascade blends into the next and the last fifth of the distance fades out. The receiver moves 1.5 texels along its
   normal (scaled by the sine of the light's angle) and half a texel toward the light.
5. **Filtering follows the sun's size**: a 16-tap blocker search over 6 texels; no blocker, no further work; otherwise
   16 comparison-sampler taps (bilinear PCF each) on a Poisson disk whose radius is the penumbra the light's angular
   radius gives the mean blocker's distance, clamped to 1.5 to 6 texels. Contact shadows are sharp, a building's shadow
   softens with distance.
6. **Beyond the last cascade nothing shadows the sun** except a surface's own facing. Kilometers out, terrain's facing
   already darkens the slopes turned from a low sun and aerial perspective lowers the contrast; the screenshots of a
   2° sun from 300 m show the fade at 800 to 1000 m reads as haze. A horizon map needs coarse terrain heights, which
   the engine does not have (they are the toolbox's, 0042): it becomes an engine seam (a far-shadow term a module
   provides, as data) when the toolbox needs it, VCH-143.
7. **The real cost is the frame's GPU span.** Passes overlap on Apple GPUs (the cascades run beside the camera's
   culling), so `VEACH_GPU_TIMINGS` now also logs `frame_ms`, the command buffer's span, next to the sum of passes.

## Consequences

- Real-time images have sun shadows at every scale: Bistro's street and trees, the slice's craft, pilot and dug
  hole, the SP-16 field's grass and canopies, terrain self-shadowing at sunset. Photo mode is unchanged.
- Cost on the M5 Max (GPU frame against the build before, 4 x 2048²): Bistro +1.0 to 1.1 ms at noon and +2.5 to
  2.8 ms at sunset, the SP-16 field +1.7 to 2.2 ms, at 1080p and 1440p; the CPU pays 1.7 to 2 ms more per Bistro frame
  for six more passes declaring every mesh buffer and texture. Above 0062's 0.4 ms estimate, and geometry-bound: a
  low sun sweeps every caster of a town into every cascade, which fewer or smaller cascades barely change. Light-side
  occlusion (last frame's cascade as a HiZ), cached far cascades and scene residency declared once are VCH-143; ADR
  0064's presets will set `SunShadows`.
- Memory: 16 MB per cascade at 2048² (64 MB for one sun, 128 MB for two).
- The goldens with a sun changed on purpose (Bistro at noon and sunset, FlightHelmet and the runtime mesh in real
  time); the thin-triangles golden turns shadow maps off to keep checking shading only.
- The penumbra filter has no temporal component; with TAA (0061) it can trade taps for rotation.
- Revisit if: shadow passes dominate frames with many cascades of static geometry (cache or stagger the far cascades,
  VCH-143), the sweep test keeps too many casters in open terrain (a tighter receiver volume), or a scene needs
  shadows past a kilometer (the horizon term).

## Alternatives considered

- **A culling and draw pass per cascade (20 dependent passes):** measured slower than four batched compute passes: the
  pipeline drains between small dependent passes cost more than the work.
- **Choosing the cascade by which box holds the point** (the finest that contains it): uses resolution better near
  the camera, but disagrees with culling by slices, so a receiver could look up a cascade whose casters were culled.
- **A set of maps per frame in flight:** no measurable gain in Bistro, 64 MB more.
- **Variance or exponential shadow maps:** filterable, but light bleeding with overlapping casters (foliage) and twice
  the memory; PCF with a blocker search is exact where it matters, at contact.
- **A depth-clamp raster state in the RHI** instead of pancaking in the vertex shader: an RHI change on two backends
  for what one `min` does.
- **Shadowing every directional light:** cost scales with suns; ADR 0062 keeps two.
- **Virtual shadow maps:** rejected by 0062.
