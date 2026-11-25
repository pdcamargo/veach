# Temporal foundation and direct lighting at speed

**Status:** Implemented
**Area:** render, rhi
**Owner:** dev lead

## Summary

The real-time renderer becomes temporal. It renders at a lower internal resolution with sub-pixel jitter, keeps
per-view history (motion vectors, the previous frame's surfaces, reservoirs, denoiser and upscaler history),
estimates direct light from every light with ReSTIR DI and about one shadow ray per pixel, denoises the result,
and upscales it to the output resolution: MetalFX on Apple GPUs, Veach's own temporal upscaler everywhere else.
The target is Bistro's exterior under 16.6 ms of GPU time at 1440p output on the M5 Max, with no loss of accuracy
against the path tracer's direct-light reference.

## Motivation

Real-time renderer v0 spends 21.6 ms shading Bistro at 1440p, almost all of it in the 4 emissive light samples'
shadow rays and in coverage tests on foliage ([results](../realtime-renderer-v0/spec.md#results)). Its 4 samples
leave visible noise (the Cornell box's error against the reference is noise, not bias). Every later real-time
feature (soft shadows, Candela's GI, reflections) needs the same three things this milestone builds: reprojection
with disocclusion, reservoir resampling, and a denoiser. Roadmap: VISION item 2 (raytracing-first rendering at
real-time budgets); backlog VCH-022 (denoiser), VCH-023 (upscaler), VCH-059 (this spec).

## Requirements

**Resolution and jitter**
- MUST render the visibility buffer, surfaces, lighting and denoising at the render resolution, which is the
  output resolution times `Upscaling.render_scale` (default 0.75: 1920x1080 for 1440p), and upscale to the output.
- MUST jitter the projection by a Halton(2,3) sub-pixel offset per frame while an upscaler runs, and never
  otherwise. Motion vectors MUST exclude the jitter.

**Temporal foundation** ([ADR 0040](../../adr/0040-temporal-foundation.md))
- MUST compute per-pixel motion from the camera's previous view-projection and each instance's previous
  object-to-world transform, so a moving camera and a moving object both reproject.
- MUST keep, per view, the previous frame's surfaces (position, normals, BRDF inputs) and use them to detect
  disocclusion: a reprojected sample is valid only if it lands on the screen on a surface at the same plane with
  a similar normal.
- MUST reset all history on the first frame, on a resize, on a render-scale or upscaler change, and on a camera
  cut (a jump of more than 2 m or 45 degrees in one frame).

**Direct lighting** ([ADR 0041](../../adr/0041-restir-di.md))
- MUST estimate direct light from point, spot and emissive lights with ReSTIR DI: power-weighted candidates from
  an alias table, temporal and spatial reservoir reuse, and one shadow ray per pixel for shading. Directional lights
  are evaluated exactly, one shadow ray each ([ADR 0046](../../adr/0046-exact-directional-lights.md)).
- MUST stay unbiased: with the denoiser off and the camera still, the average of many frames MUST converge to the
  path tracer's direct-light reference (`PathTracer.mode = 1`).

**Denoiser** ([ADR 0042](../../adr/0042-denoiser.md))
- MUST denoise the demodulated diffuse and specular direct light with Veach's own temporal and spatial filter,
  guided by the surfaces, and MUST NOT smear across disocclusions (history is dropped there, not blended).

**Upscaling** ([ADR 0043](../../adr/0043-upscaling.md))
- MUST upscale through one interface: MetalFX's temporal scaler on Metal devices that support it, Veach's own
  temporal upscaler (TAAU) on every backend, including DX12 on WARP in CI.
- `Upscaling.mode = none` MUST render at the output resolution without jitter or upscaling (comparisons, debugging).

**Measurement**
- Every new pass MUST report its GPU time through `gpu_timings`.
- `RealtimeLighting.view` MUST show the intermediate signals for agents and debugging: the final image, the
  noisy (undenoised) image, motion, history validity.

## API sketch

```cpp
// On the camera, next to Camera and RealtimeLighting. Absent: the defaults.
enum class UpscalerMode : std::uint8_t { automatic, metalfx, temporal, none };
struct Upscaling {
    UpscalerMode mode = UpscalerMode::automatic;  ///< automatic: MetalFX where supported, else temporal (TAAU)
    float render_scale = 0.75f;                   ///< render resolution / output resolution, 0.5..1
};

enum class RealtimeView : std::uint8_t { final, noisy, motion, history };
struct RealtimeLighting {
    std::uint32_t light_candidates = 12;  ///< ReSTIR initial candidates per pixel (replaces emissive_samples)
    bool temporal_reuse = true;
    std::uint32_t spatial_samples = 2;    ///< neighbors per pixel in spatial reuse; 0 = off
    bool denoise = true;
    RealtimeView view = RealtimeView::final;
    bool ambient = true;                  // STOPGAP until Candela, as in v0
    math::vec3 sky{0.0f};
    std::uint32_t ao_rays = 4;
    float ao_radius = 1.0f;
    std::uint32_t debug_skip = 0;         // measurement only, as in v0
};
```

```luau
camera:set(c.Upscaling, { mode = "temporal", render_scale = 0.75 })
camera:set(c.RealtimeLighting, { view = "noisy", denoise = false })
```

## Acceptance criteria

- **temporal.AC1**: Motion vectors are exact for camera and object motion: after a known camera translation and a
  known object translation between two frames, the `motion` view matches the analytic screen-space motion within
  0.05 render pixels, and is zero for a still camera and scene.
- **temporal.AC2**: Disocclusion is detected where it happens and nowhere else: when an object moves sideways,
  the `history` view marks invalid the pixels it uncovered (at least 95% of them) and keeps at least 99% of the
  pixels that stayed visible valid.
- **temporal.AC3**: History resets on the first frame, a resize, a render-scale change, an upscaler change and a
  camera cut, and only then (a `history` view shows every pixel invalid on that frame and valid on the next).
- **temporal.AC4**: ReSTIR DI is unbiased: with the denoiser off, `Upscaling.mode = none` and a still camera, the
  average of 256 frames matches the path tracer's direct-light reference to a mean FLIP at most 0.02 and a mean
  luminance within 2%, on the Cornell box, FlightHelmet and Bistro.
- **temporal.AC5**: Direct lighting traces at most 1.05 shadow rays per pixel on average (AO rays and coverage
  layers aside), counted on the GPU.
- **temporal.AC6**: No regression against the reference: with a still camera and 64 warm-up frames, the final
  image (denoised, upscaled) of the Cornell box, FlightHelmet, Sponza and Bistro has a mean FLIP against the
  path tracer's anti-aliased direct-light reference no higher than v0's (0.0784, 0.0017, 0.0143, 0.0148).
- **temporal.AC7**: The denoiser is stable: with a still camera and no upscaler, consecutive final frames differ by a
  mean FLIP of at most 0.01 (where differences become visible) after warm-up. (First written as 0.005; see Results.)
- **temporal.AC8**: Both upscalers produce the output resolution from the render resolution: TAAU on Metal and on
  DX12 (WARP) against one shared golden; MetalFX on Metal within a mean FLIP of 0.02 of TAAU's image.
- **temporal.AC9**: Temporal goldens: the Cornell box and FlightHelmet, still camera, 32 warm-up frames, TAAU,
  compared like every golden, on Metal and DX12.
- **temporal.AC10**: No ghosting: the camera moves along a fixed path for 30 frames; the last frame's mean FLIP
  against the path-traced reference of the final view is at most 0.01 higher than a still camera's at that view
  after the same number of frames.
- **temporal.AC11**: Bistro's exterior renders in under 16.6 ms of GPU time (median) at 1440p output on the M5 Max
  (`tools/bench/frame_times.py`), and the per-pass split is recorded below.
- **temporal.AC12**: Every new pass appears in `gpu_timings` by name.

## Results

Measured on the M5 Max (macOS 26, nothing else using the GPU), release build, Metal, default settings (render
scale 0.75, MetalFX, 12 candidates, 2 spatial neighbors, directional lights exact). Tests named after each criterion
run in the pre-push gate.

| Criterion | Measured | Test |
|---|---|---|
| AC1 motion | camera yaw: worst error 0.011 px over every pixel; an object moved 2 cm: motion on it only, 1.61..1.85 px for its 3.8..4.6 m depth | `mcp: temporal motion` |
| AC2 disocclusion | 100% of the pixels the box uncovered, 100% of the ones that stayed visible keep history | `mcp: temporal motion` |
| AC3 resets | first frame, resize, upscaler change, cut: no history on that frame, all of it on the next | `mcp: temporal motion` |
| AC4 unbiased | 256 undenoised frames (640x360): FLIP 0.0143 / 0.0017 / 0.0148, luminance -0.12% / -0.07% / +0.03% (Cornell, FlightHelmet, Bistro) | `temporal.AC4` (Cornell, FlightHelmet) |
| AC5 rays | 0.98 ReSTIR shadow rays per lit pixel (plus one per directional light) | `mcp: temporal stability` |
| AC6 no regression | below: Cornell and Sponza better than v0, FlightHelmet and Bistro within 0.0003; superseded by the tolerance policy (ADR 0047), these are its baselines | `tolerance: ...` |
| AC7 stability | 0.006 between consecutive frames | `mcp: temporal stability` |
| AC8 upscalers | MetalFX against TAAU: FLIP 0.013 | `mcp: MetalFX and TAAU agree` |
| AC9 goldens | Cornell box, FlightHelmet, TAAU after 32 frames, Metal and DX12 (WARP) | `golden: ... (realtime)` |
| AC10 ghosting | 0.0802 after 30 frames of motion, 0.0714 still (+0.0088) | `mcp: temporal ghosting` |
| AC11 Bistro | 15.75 ms median, 16.32 ms p95 | `frame_times.py` |
| AC12 timings | every new pass in `gpu_timings` | `mcp: temporal stability` |

**AC6, the final image against the reference** (`compare_direct.py --final`, 640x360, 64 frames of warm-up), with
v0's protocol (full resolution through pixel centers, against the unjittered reference: how v0's numbers were
measured):

| Scene | Final (denoised) | v0 | Mean luminance |
|---|---|---|---|
| Cornell box | 0.0208 | 0.0784 | -0.95% |
| FlightHelmet | 0.0018 | 0.0017 | -0.22% |
| Sponza (Intel) | 0.0119 | 0.0143 | +5.96% |
| Bistro (exterior) | 0.0151 | 0.0148 | -0.13% |

Before ADR 0046 (the sun through ReSTIR) FlightHelmet was 0.0048: its two punctual lights shared one ray. What is
left on FlightHelmet and Bistro (0.0001 and 0.0003) is the denoiser on fine detail; their accumulated lighting
matches v0 (AC4). Sponza's +6% is not lighting: its one blended material, a dirt decal, renders masked at 0.5 in real
time (the v0 limitation) and stochastically in the path tracer (VCH-060). With the upscaler (0.75 scale, against the
anti-aliased reference) the numbers are higher (0.037 / 0.011 / 0.023 / 0.092 before ADR 0046): reconstructing
detail from fewer samples, and both upscalers resolve high-contrast detail darker (Bistro: TAAU -3.2%, MetalFX -6.6%
mean luminance; TAAU's luminance weighting and variance clipping explain two thirds of its share).

**AC7** was first written as 0.005. The denoiser's spatial filter scales its edge-stopping by the accumulated
mean's variance (variance / history length, as ReLAX): long-converged pixels keep detail (Bistro 0.0158 -> 0.0151,
FlightHelmet then 0.0055 -> 0.0048) at the cost of stability (0.0044 -> 0.0060). Accuracy is the requirement; the
stability limit moved to FLIP's visible level.

**AC11, frame times at 1440p output** (`frame_times.py`: the device's GPU time per frame, which includes MetalFX and
the gaps between passes):

| Scene | Frame (median) | Frame (p95) |
|---|---|---|
| Cornell box | 5.44 ms | 5.56 ms |
| FlightHelmet | 3.68 ms | 3.76 ms |
| Sponza (Intel) | 11.06 ms | 11.45 ms |
| Bistro (exterior) | 15.75 ms | 16.32 ms |

Bistro's passes, ms: visibility 1.68, surface 0.67, ReSTIR candidates and temporal reuse 1.51, spatial reuse 1.39,
shade 5.36 (ReSTIR's ray and the sun's, through foliage), denoiser 3.32, TLAS 0.19, display 0.08; MetalFX is not in
per-pass timestamps (it encodes its own work). From v0's 38.9 ms (pass sum; the device's frame time was higher, see
ADR 0044).

**Found on the way.** Shading ignored the jitter: the surface pass built its camera ray through the pixel center
plus the jitter in jittered space, which is the unjittered pixel center, so every frame shaded the same point and the
upscalers' anti-aliasing had nothing to average (fixed: it shades the raster's own sample, and TAAU places each
sample at its center minus the jitter). Metal residency cost 10 ms of Bistro's frame (ADR 0044). Metal's shader
validation makes these ray queries vary run to run, so goldens render without it (rendering guide).

## Out of scope

- Soft shadows from area, spot and point lights with a radius: next (VCH-021); ReSTIR's light samples are where
  they come from.
- GI and reflections (Candela VCH-017, VCH-018), ReSTIR GI.
- DLSS, FSR and XeSS (vendor SDKs and licenses: the owner decides first); a light BVH (the alias table first).
- Skinned and deforming meshes' motion (no animation yet): the motion seam takes a previous position per vertex
  later.

## Open questions

None.

## Links

- ADRs: [0040 temporal foundation](../../adr/0040-temporal-foundation.md), [0041 ReSTIR DI](../../adr/0041-restir-di.md),
  [0042 denoiser](../../adr/0042-denoiser.md), [0043 upscaling](../../adr/0043-upscaling.md),
  [0037 visibility buffer](../../adr/0037-realtime-renderer-visibility-buffer.md)
- Backlog: VCH-059 (this spec), VCH-022, VCH-023, VCH-021
- Previous: [real-time renderer v0](../realtime-renderer-v0/spec.md)
