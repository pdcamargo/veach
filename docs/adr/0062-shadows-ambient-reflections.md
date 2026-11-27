# 0062: Shadows, ambient light and reflections without ray tracing

Date: 2026-11-27

## Status

Accepted.

## Context

ADR 0040's interim shading had no shadows, a 1×1 sky-irradiance texel as ambient on the diffuse lobe, and no
reflections, so metals looked dark. The real-time renderer must stay a rasterizer with no feature that needs ray tracing
([0039](0039-realtime-renderer-is-raster-pbr.md), [0003](0003-platforms-and-hardware-target.md)). The scenes range from
a cockpit and a base to a planet with several suns, eclipses and rings, at any body scale. A 1×1 upward irradiance is
wrong for walls and for any normal on a curved planet. Research: [08-renderer](../research/08-renderer.md).

## Decision

**Shadows: cascaded shadow maps near, analytic occluders far.**

- **Cascaded shadow maps** for at most two suns: the two brightest by illuminance at the camera after occlusion
  ([0048](0048-bodies-on-rails.md)). Other suns light the scene unshadowed by geometry but still shadowed by the
  analytic occluders below. Cascades are stable (bounding-sphere fit, texel-snapped), camera-relative
  ([0045](0045-large-world-positions-and-reference-frames.md)) and their split distances are meters that never follow
  scale. Count and size are preset values ([0064](0064-quality-presets.md)): 3 × 2048² on the M3 Pro-class preset,
  2 × 1024² on the M1-class preset. Filtering is PCF with hardware comparison samplers and depth bias (VCH-098); until
  VCH-098 lands, manual PCF on a depth texture. Cascades need texture arrays (VCH-072) or an atlas until then.
- **Analytic occluders** for everything planet-sized: exact, cheap and scale-free, evaluated per pixel in the shade
  and forward passes and by the atmosphere, and stored as records in the shading-contribution buffers of
  [0060](0060-gpu-driven-visibility-buffer.md), so modules can add their own:
  - the body's own night side, through the atmosphere's transmittance LUT ([0063](0063-astronomical-rendering.md));
  - eclipses, as a per-sun **visible fraction** (the overlap of the sun's disk with occluding spheres), computed on the
    CPU at extraction from the system's occluder query ([0048](0048-bodies-on-rails.md));
  - ring shadows, as a ray-plane intersection into the ring's radial transmittance texture.
- **Not decided here:** a cloud shadow map (with the cloud feature), far-terrain self-shadowing at tens of kilometers
  (deferred), shadowed local lights (clustered local lights are unshadowed for now). No virtual shadow maps.

**Ambient light.**

- **Sky ambient as L2 spherical harmonics**, projected from the sky-view LUT every frame per lit sky (one set for the
  camera's body). It replaces the 1×1 irradiance texel and is evaluated with the shading normal.
- **GTAO** at half resolution with temporal reuse ([0061](0061-temporal-accumulation-no-upscaling.md)); its radius is
  meters and never scales.
- **Planetshine and moonlight:** a dim directional light per nearby lit body, from its albedo and phase, added to the
  light list. It is data, not a shader change.
- Emission is seen directly; emissive surfaces do not light other surfaces (no GI).

**Reflections.**

- A **prefiltered sky cubemap** rebuilt from the sky LUT when the sky changes beyond a threshold (needs cube maps,
  VCH-072), used for the specular lobe everywhere.
- **Local reflection probes** captured at bases, caves and ship interiors, placed by content or by modules, relit only
  approximately when the sky changes; probe spacing is meters.
- **Screen-space reflections** as an M3 Pro-class preset option for water and wet surfaces, falling back to probes and
  the sky cubemap off-screen. Not on the M1-class preset.

**Kept from 0040:** a light behind the geometric surface contributes nothing (normal maps cannot leak light), and a
shading normal facing away from the camera is bent back toward it.

**Reference.** Shadows, ambient and reflections are checked by eye against photo mode's path-traced image of the same
view ([0041](0041-photo-mode.md)); a stylized raster image is not expected to converge to it.

## Consequences

- Real-time images gain sun shadows, eclipses, ring shadows and occlusion without any ray tracing.
- Occlusion of the sun at planetary scale never depends on shadow-map resolution, because it is analytic.
- Indoor and cave lighting depends on probes and GTAO; caves without probes are lit by sky SH through GTAO only, which
  is too bright deep inside. Placing probes is content work, and auto-placement is a later module.
- Costs (estimates, worst-case scene): cascades 0.4 ms on the M5 Max at 1080p, 2.5 ms on an M1-class GPU, 1.1 ms on
  M3 Pro-class at 1440p; GTAO 0.1, 0.7 and 0.5 ms. SH projection and analytic occluders are below 0.05 ms.
- **Validation:** VCH-086 (SP-10): two suns and an eclipse, with screenshots and Tracy ms, and an eclipse's visible
  fraction matching the disk-overlap formula in a unit test. VCH-091 (SP-15): the landed scene of the first slice by
  eye. VCH-097 (SP-21): cascade and GTAO cost on real M1-class and M3 Pro-class machines. VCH-098 is the RHI
  prerequisite.
- **Reopen if** stable cascades cannot cover a base or landing site without shimmering on the M1-class preset, if SH
  ambient on curved terrain looks clearly wrong next to photo mode, or if real-machine measurements put cascades above
  their share of the budget (a horizon map from coarse terrain would then take over the far cascade).

## Alternatives considered

- **Ray-traced shadows or reflections.** Ruled out by [0039](0039-realtime-renderer-is-raster-pbr.md) and the
  hardware floor.
- **Virtual shadow maps.** Indirect draws, page tables and feedback; wind-animated foliage invalidates cached pages.
  Too much for the M1 class and for VCH-071's first scope.
- **Shadow maps for planets and moons.** Resolution-bound and scale-dependent where the analytic answer is exact.
- **Baked light probes for ambient (irradiance volumes).** A generated, editable world has nothing to bake against
  ahead of time; the sky dominates outdoors and SH captures it each frame.
- **SSR on every preset.** Too costly for the M1 class, and wrong off-screen without probes behind it.
