# 0051: Candela: a ReSTIR GI final gather over a world-space radiance cache

Date: 2026-11-25

## Status

Proposed (the owner decides: review packet in the 2026-11-25 summary). No Candela code until then.

## Context

Candela ([spec](../specs/candela/spec.md), VCH-017) must give the real-time renderer diffuse indirect light inside
the Candela slice of [0049](0049-frame-budget-and-quality-tiers.md): 4.5 ms in Performance (1440p output, 0.5
render scale: 1280x720 = 0.92 Mpixels) and 10 ms in Quality (0.75 scale: 1920x1080 = 2.07 Mpixels), with nothing
baked, and comparable to the path tracer within [0047](0047-image-tolerance-policy.md).

**What a ray costs here.** Measured on the M5 Max, release build, Bistro at noon, Performance resolution
(`tools/bench/frame_times.py`; the path tracer through the same scene, 1 sample per pixel, no jitter):

| Work per render pixel | GPU time |
|---|---|
| One long visibility ray (to 10 km), the sky looked up on a miss: the ambient stopgap with 1 ray | 0.87 ms |
| Four of them | 2.94 ms |
| One fully shaded bounce: ray, hit, material, a light sample and its shadow ray (path tracer, 1 -> 2 bounces) | 4.64 ms |
| The next bounce (2 -> 3) | 3.99 ms |
| The sky's LUT passes, all of them | under 0.25 ms (each under the report's 0.05 ms floor) |

So a fully shaded GI ray at every pixel takes the whole Performance slice by itself, before reuse, caching or
upsampling. Whatever Candela is, it shades far fewer than one hit per pixel per frame, or shades hits cheaply.

**What we already have** and want Candela to use: ReSTIR's reservoirs and reuse (0041), the temporal foundation's
surfaces, motion and disocclusion (0040), a denoiser that takes a demodulated diffuse signal (0042), the physical
sky for rays that leave the scene (0050), and a path tracer whose bounce code is the reference.

## Decision (proposed)

A hybrid in the shape of AMD's GI-1.0, NVIDIA's SHaRC and Lumen's final gather, built from our parts:

1. **World-space radiance cache.** A spatial hash grid (cells sized by distance to the camera) holding each cell's
   outgoing diffuse radiance. Updated every frame by tracing short paths from a sparse subset of pixels (one
   random pixel per 5x5 block, about 4%, as SHaRC recommends), with the path tracer's bounce code: light sampling
   at each vertex, the sky on misses, the cache itself read at the path's end for further bounces. Cells not
   touched for a while are evicted. Infinite bounces converge over frames.
2. **Final gather: ReSTIR GI.** At the render resolution, a quarter of the pixels (Performance; half in Quality)
   trace one cosine ray from the surface each frame. The hit's radiance is read from the cache (no material
   evaluation at the hit beyond its normal), or the sky on a miss. Samples become GI reservoirs, reused temporally
   and spatially (Ouyang 2021) with our ReSTIR code, then upsampled to every pixel.
3. **Denoising:** the gathered indirect irradiance joins the diffuse signal the denoiser already filters; no second
   denoiser.
4. **Reference:** `PathTracer.mode = 4`, direct light plus diffuse indirect light (no specular after the first
   hit): what Candela estimates.
5. **The stopgap goes:** the ambient + AO path remains only as Candela's fallback when its `Candela` component is
   absent.

**Cost estimate, Performance (from the table):** gather 0.25 x 0.9-1.2 ms (visibility-class rays, cache reads) =
0.3 ms, up to 1.2 ms if hits need full shading; cache update 4% of pixels x 2-3 shaded bounces = 0.4-0.6 ms; GI
reservoir reuse at quarter rate 0.3-0.5 ms (DI's two reuse passes are 0.9 ms at full rate); upsampling and cache
upkeep 0.3 ms. **Total 1.3-2.6 ms of 4.5 ms.** Quality at half rate and 2.25x the pixels: about 4-7 ms of 10 ms.
These are estimates from measured parts, not a prototype: the first implementation step is to measure the gather
and the cache update alone and stop if either is twice its estimate.

## Consequences

- One code path for bounces: the cache's update reuses the path tracer's code, so Candela and the reference cannot
  drift apart in materials, lights or sky.
- Reflections (VCH-018) can read the same cache for rough reflections and ray termination.
- Two new pieces of GPU state: the hash grid (a fixed-size buffer, around 64 MB at 4M cells of 16 bytes) and the
  GI reservoirs (one per gathered pixel).
- The cache lags behind sudden changes (a light switched off) by its update rate: candela.AC5 bounds it at 10 frames.
- Hash cells are coarse: detail at contact comes from the per-pixel gather, not the cache.

## Alternatives considered

Measured costs from other engines run on other hardware: use them as ratios, not as milliseconds here.

| | Cost vs the 4.5 ms slice | Bistro and Sponza | Stability | Uses our foundations | Lock-in |
|---|---|---|---|---|---|
| **ReSTIR GI alone** ([Ouyang 2021](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing): 8-18 ms in UE4 at 1080p on an RTX 3090) | Over: one shaded hit per pixel is 4.6 ms here before reuse | Sharp contact, one bounce only: Sponza's arcades, lit by bounced light, stay too dark | Boils without heavy denoising; disocclusions start from nothing | ReSTIR, temporal, denoiser | Low |
| **DDGI** ([Majercik 2019](https://research.nvidia.com/index.php/publication/2019-05_dynamic-diffuse-global-illumination-ray-traced-irradiance-fields): about 1 ms diffuse on a 2080 Ti at 1080p) | Fits (probe rays are few and independent of resolution) | Low frequency; leaks through Bistro's thin walls and foliage; probe relocation needed in dense geometry | Very stable, lags by its hysteresis | Sky only; its own volumes, not our reservoirs or denoiser | Probe volumes become something scenes carry |
| **Surfels** ([EA GIBS, 2021](https://www.ea.com/seed/news/siggraph21-global-illumination-surfels)) | Fits in Frostbite; no public numbers | Good on surfaces; surfel coverage of foliage is hard | Stable once surfels settle | Little: its own spawning, recycling and acceleration structure | High: the most custom machinery |
| **Radiance cache alone** (hash grid read at the primary hit) | Cheapest, under 1 ms | Blocky: cell-sized lighting, no contact detail | Stable | Path tracer code | Low |
| **Hybrid: screen probes + world cache** ([GI-1.0](https://ar5iv.labs.arxiv.org/html/2310.19855): direct + indirect in 3.5-4.2 ms at 1080p on an RX 6900 XT; Lumen: [4 ms at 60 fps](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine) for GI and reflections) | Fits | Good | Good | Cache yes; screen probes are a new structure next to our reservoirs | Low |
| **Hybrid: ReSTIR GI gather + world cache** (this decision; cache after [SHaRC](https://github.com/NVIDIAGameWorks/Spatial-Hash-Radiance-Cache/blob/main/docs/Integration.md)) | Fits, 1.3-2.6 ms estimated | Good: per-pixel contact, cached multiple bounces | Reuse plus our denoiser; the cache steadies the gather | All of them | Low |

**Screen probes against ReSTIR GI for the gather** is the closest call: GI-1.0's probes (one per 8x8 tile) trace
fewer rays and are proven at this cost. ReSTIR GI wins here because its reservoirs, reuse and validation are code
we already run for direct light, and it converges to the path tracer's answer instead of an interpolated one,
which is what ADR 0047 measures. If the gather costs twice its estimate, screen probes are the fallback.
