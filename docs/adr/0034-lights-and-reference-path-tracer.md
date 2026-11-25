# 0034: Lights in physical units, and a reference path tracer

Date: 2026-11-25

## Status

Accepted

## Context

Candela, Veach's real-time GI ([VISION](../VISION.md)), will be judged against the light transport it
approximates. That needs a renderer that is slow but right: unbiased, progressive, deterministic, and on both
backends. It also needs lights defined in units that the reference and every later renderer interpret the
same way.

## Decision

**Lights are components on entities**, oriented by `GlobalTransform` (they shine down −Z, as glTF's
`KHR_lights_punctual` does). Units are photometric, the glTF ones:

| Component | Fields | Unit |
|---|---|---|
| `DirectionalLight` | `color` (linear RGB), `illuminance` | lux |
| `PointLight` | `color`, `intensity` | candela |
| `SpotLight` | `color`, `intensity`, `inner_angle`, `outer_angle` (radians, KHR smooth falloff) | candela |
| emissive surfaces | `evalEmission` of the material ([ADR 0033](0033-material-model.md)): glTF `emissiveFactor × emissiveStrength` | nits (cd/m²) |

The camera's `ev100` sets exposure: the pixel is `L / (1.2 · 2^ev100)`, clamped, then sRGB-encoded. Lights are
punctual (no radius) in v0.

**The reference path tracer** (`render::PathTracerFeature`, compute with inline ray queries, `RayQuery` /
Metal intersection queries):

- One camera ray per pixel per sample, jittered inside the pixel.
- At each hit: `evaluateSurface`, then the glTF metallic-roughness BRDF: Lambert diffuse weighted by
  `(1 − F)(1 − metallic)`, and GGX specular with Smith height-correlated G and Schlick F, `F0 = mix(0.04, base
  color, metallic)`. Roughness is clamped to ≥ 0.02.
- **Next-event estimation:**
  - One light per bounce: a punctual light, or an emissive triangle picked in proportion to area × the
    material's emission weight, with a uniform point on it. The emission there is evaluated by the material,
    so procedural emitters work.
  - Shadow rays use the same alpha rules as primary rays.
- **MIS** (power heuristic) between light sampling and BSDF sampling (GGX VNDF + cosine, chosen per lobe
  weight) for emissive triangles. Punctual lights are delta, so they come from NEE only.
- **Alpha:** `MASK` tests the cutoff, and `BLEND` is stochastic (a hit with probability α). Both happen in
  the ray query's candidate loop, through `evalBaseColor`.
- **Russian roulette** after 3 bounces, `max_bounces` cap (default 8). A miss returns the `sky` radiance
  (constant, nits).
- **RNG:** PCG hash of (pixel, sample index, dimension). The same sample index gives the same random numbers
  on every backend, so differences come from floating point only.
- **Progressive accumulation** in an `rgba32_float` texture. The sample index is global. Accumulation
  restarts when the view, the target size, the `PathTracer` settings or the scene change (a hash of instances,
  materials and lights computed during extraction), or on request (MCP `path_trace`).
- The scene on the GPU: one BLAS per mesh (opaque unless a slot is not), one TLAS rebuilt per frame, and
  instance, material and light records in structured buffers, all reached through `DescriptorHandle`s
  ([ADR 0024](0024-resource-binding-model.md)).
- **Enabled per camera** by a `PathTracer { samples_per_frame, max_bounces, sky }` component (Scriptable).

**Goldens from the reference** ([ADR 0028](0028-golden-images-across-backends.md)) render at 256×256 with a
fixed sample count, on Metal and on DX12/WARP. Identical RNG streams make the noise mostly identical across
backends. Their tolerance is set per test and written down with its reason, like any loosened tolerance.

## Consequences

- One unambiguous meaning of a light or an emissive surface for every later renderer, and a ground-truth image
  for any scene on demand (MCP `path_trace`).
- Slow by design: a frame is `samples_per_frame` full paths per pixel. Interactive use is "look, then wait".
- Punctual lights and constant sky only. Area lights other than emissive meshes, IBL, volumes, spectral
  rendering and caustics through delta lights are out of v0. Each is an addition, not a redesign.
- The reference's correctness rests on its tests: white-furnace and convergence checks join the goldens.

## Alternatives considered

- **Radiometric units (watts):** most engines and glTF use photometric units, and physically based exposure
  (EV100) is defined on them.
- **A CPU reference (e.g. Embree, or our own BVH):** independent of GPU bugs, but a second scene representation
  and a second BVH. The GPU path tracer shares the asset pipeline it is meant to validate and runs where the
  goldens run.
- **Ray-tracing pipelines (hit shaders, SBTs) instead of inline queries:** better for many materials later, but
  a second binding model now. Inline queries match ADR 0024 and the first-light shaders.
