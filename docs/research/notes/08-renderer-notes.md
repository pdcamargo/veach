# Q8 renderer: raw research notes

Raw notes behind [08-renderer.md](../08-renderer.md). Collected 2026-11-27 by the dev lead with three parallel web
researchers and one local measurement session on the M5 Max. Everything marked **UNVERIFIED** comes from memory or
from a secondary source that was not checked in this session. The web-search quota ran out partway through, so
shadows, SSR and a few precedents are thinner than the rest. The notes say so wherever that happens.

## Local measurements (M5 Max, interim visibility-buffer path)

### Takeaway
On the M5 Max the interim path's per-pixel work is small: about 0.8 ms of shading at 1080p and 1.0 to 1.5 ms at
1440p. The cost is the geometry pass on big scenes, which has no culling at all: 3.8 to 7.2 ms on Bistro, a scene
whose time hardly changes with resolution. The physical sky LUTs together cost 0.1 to 0.25 ms.

### Cited findings
- Machine: Apple M5 Max, **32-core GPU** (`system_profiler SPDisplaysDataType`), Metal 4, macOS 26 (Darwin 25.3).
- Method: `build/debug/apps/lab/veach-lab --mcp --headless --rhi metal --script scripts/<scene>`, with
  `MTL_SHADER_VALIDATION=0`. The window was resized with a Luau system, 60 frames were stepped, then MCP `gpu_timings`
  was read. Debug build (shaders are compiled the same way; this matters only for the CPU). The harness was
  `scratchpad/mcp_time.py`, a throwaway script, not committed. Two runs per configuration.
- Results: pass GPU times in ms, and `last_frame_gpu_ms` (the device frame time, [ADR 0028](../../adr/0028-metal-residency-set.md)).

| Scene | Resolution | visibility | sky (view + AP + irradiance) | shade | display | frame |
|---|---|---|---|---|---|---|
| Bistro exterior | 1920x1080 | 5.67 / 6.98 | 0.17 / 0.25 | 0.80 / 0.73 | 0.19 / 0.09 | 6.85 / 8.09 |
| Bistro exterior | 2560x1440 | 7.18 / 3.81 | 0.24 / 0.13 | 1.37 / 1.03 | 0.15 / 0.19 | 8.97 / 5.18 |
| Sponza | 1920x1080 | 1.07 / 1.06 | 0.06 / 0.06 | 0.83 / 0.83 | 0.05 / 0.05 | 2.02 / 2.01 |
| Sponza | 2560x1440 | 1.21 / 1.15 | 0.06 / 0.06 | 1.46 / 1.46 | 0.08 / 0.08 | 2.81 / 2.76 |
| FlightHelmet | 1920x1080 | 0.09 / 0.09 | — | 0.06 / 0.06 | 0.04 | 0.21 / 0.20 |
| FlightHelmet | 2560x1440 | 0.08 / 0.08 | — | 0.11 / 0.11 | 0.07 | 0.27 / 0.27 |

- The visibility pass draws every meshlet of every instance with no frustum, occlusion or backface culling
  (`engine/render/src/realtime_renderer.cpp`: `cull = none`, one `draw(124*3, meshlet_count)` per instance).
  Bistro's visibility time is therefore geometry-bound and noisy: it varies by about 2x run to run, which is probably
  GPU clock and power state (**UNVERIFIED**).
- The shade pass is the one per-pixel compute pass. It reconstructs the surface, evaluates materials, and lights the
  pixel with every light (unshadowed) plus sky ambient and aerial perspective. It scales close to linearly with
  pixel count: Sponza 0.83 → 1.46 ms for 1.78x the pixels.
- A stale `veach-lab` MCP server still attached to this session ran a pre-reset binary (ReSTIR and denoiser passes).
  Its numbers were discarded.

### Inferences
- Culling (frustum, cone and Hi-Z on meshlets) is the first win for any base renderer, whichever one we pick. Wihlidal
  culled 78% of triangles in his test scene.
- Doubling the shade cost for real lighting (shadows, clustered lights, GTAO, IBL) still fits comfortably on the
  M5 Max. The M1 is the question.

### Gaps
- No Tracy capture: these are render-graph pass timestamps only.
- Release-build GPU numbers and clocks under sustained load were not measured.

## 1. Base renderer: visibility buffer, clustered forward, deferred

### Takeaway
A visibility buffer beats a G-buffer on bandwidth, and the gap grows with resolution and MSAA. It costs us nothing new
because the path already exists. Translucency must go through a forward pass in every precedent (The Forge, Nanite).
Clustered forward (id Tech) is the strongest alternative, and its argument is decals and MSAA, not speed.

### Cited findings
- Burns & Hunt 2013, "The Visibility Buffer: A Cache-Friendly Approach to Deferred Shading" (JCGT 2(2)): the buffer
  stores triangle and instance ids per pixel, so hidden surfaces never sample textures —
  [JCGT](https://jcgt.org/published/0002/02/04/paper.pdf). Follow-up: Schied & Dachsbacher 2015, deferred attribute
  interpolation — [KIT](https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf).
- Wihlidal, GDC 2016, "Optimizing the Graphics Pipeline with GPU Compute" (Frostbite) —
  [slides](https://www.slideshare.net/slideshow/optimizing-the-graphics-pipeline-with-compute-gdc-2016/59747720),
  [GDC Vault](https://gdcvault.com/play/1023109/Optimizing-the-Graphics-Pipeline-With):
  - Test scene: 443,429 triangles at 1080p. 78% of triangles were culled. Exclusive / inclusive by test:
    orientation 46/46%, depth 42/20%, small primitive 30/8%, frustum 8/4% (scene dependent).
  - Draw time without culling: XB1 5.47 ms, PS4 4.56 ms, Fury X 0.79 ms. With culling the draw falls to about 4.5,
    3.8 and 0.47 ms; the culling itself costs 0.24 to 0.26 ms (XB1), 0.13 to 0.15 ms (PS4) and 0.06 ms (PC). The
    table layout was garbled in extraction.
  - Clusters of 256 triangles are the stated sweet spot; at 64 the command processor becomes the bottleneck.
  - Cluster tests: cone (backface), sphere (frustum), screen-space box against Hi-Z. Per-triangle tests: orientation,
    zero area, small primitive, frustum, depth.
  - The talk does not cover alpha test. A forum thread notes the benchmark ran without cluster culling —
    [gamedev.net](https://gamedev.net/forums/topic/677240-per-triangle-culling-gdc-frostbite/5282984/).
- Engel / ConfettiFX, GDC 2016, "Filtered and Culled Visibility Buffer" —
  [slides](https://www.slideshare.net/slideshow/triangle-visibility-buffer/238860894). Visibility buffer vs deferred,
  frame ms at 4K:
  - R9 380: no MSAA 15.19 vs 20.19; 2x 25.87 vs 42.68; 4x 37.86 vs 69.64.
  - GTX 970: 13.52 vs 16.82; 2x 19.87 vs 26.27; 4x 32.68 vs 48.12.
  - Xbox One 1080p: 19.78 vs 24.77; 2x 28.00 vs 46.41.
  - Resolution scaling, R9 380: 8.57 / 10.72 / 15.19 (1080p / 1440p / 4K) vs deferred 9.75 / 12.30 / 20.19.
  - Stage split, R9 380 at 4K: culling 2.66, fill 5.07, shade 3.43, vs deferred fill 12.19 and shade 1.29. Shading
    the visibility buffer under 4x MSAA rises to 15.58.
  - Memory at 4K with 4x MSAA: 276.6 MB vs 635.3 MB.
  - Alpha test is one mask bit per pixel, with opaque and masked batches drawn separately. Transparency is a forward
    pass sorted back to front. Material divergence is not discussed.
- The Forge v1.57 (May 2024): TVB 1.0 is one unified indirect draw with no DrawID, and its "Forward++" shading uses a
  tiled light list. TVB 2.0 (prototype) fills depth and the visibility buffer from two compute dispatches with no
  draws, and runs on macOS/iOS. An OIT extension stores triangle ids in per-pixel linked lists —
  [release notes](https://github.com/ConfettiFX/The-Forge/releases/tag/v1.57).
- Nanite (Karis, Stubbe, Wihlidal, SIGGRAPH 2021) —
  [deck](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf), not fetched
  (too large); details from secondary sources:
  - The visibility buffer is written with a 64-bit atomic max of `depth<<32 | id` —
    [Arm blog](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-and-unreal-engine-s-nanite-enabling-the-future-of-mobile-graphics).
  - Metal's 64-bit atomic min/max reportedly needs Apple8 (M2); the M1 lacks it —
    [UE forum](https://forums.unrealengine.com/t/lumen-nanite-on-macos/508411/53) (**UNVERIFIED**, forum).
  - Nanite supports Opaque and Masked only: no translucency, MSAA or forward shading —
    [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine).
  - Material divergence: UE 5.0 drew a full-screen pass per material with material-id-as-depth (tiled 64x64). UE 5.4
    sorts pixels into compute shading bins (quads kept for derivatives); one scene had 3,779 bins, 81% empty, and
    compacting them saved about 1 ms — [notes on Wihlidal GDC 2024](https://scthe-github-io.vercel.app/blog/nanite-materials-notes).
- id Tech: Doom Eternal is fully forward (clustered), with no G-buffer. A hybrid per-fragment bin picks the shorter of
  the cluster and tile lists. Decals are binned too (up to 1M per level); the stated reason for forward is that
  G-buffer decal blending was too slow — [SIGGRAPH 2020](https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf)
  (from search snippets), [Doom 2016 study](https://adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study).
- Olsson, Billeter & Assarsson, HPG 2012, "Clustered Deferred and Forward Shading": clusters by 3D view position
  handle far more lights than tiles — [preprint](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf).
- Drobot, SIGGRAPH 2017 (Call of Duty: Infinite Warfare): z-binning, where lights are binned per depth slice and
  combined with 2D tiles, which is cheaper in memory than full 3D clusters —
  [deck](https://www.advances.realtimerendering.com/s2017/2017_Sig_Improved_Culling_final.pdf). Activision 2023, large
  terrain rendering — [Advances 2023](https://advances.realtimerendering.com/s2023/Etienne%28ATVI%29-Large%20Scale%20Terrain%20Rendering%20with%20notes%20%28Advances%202023%29.pdf).
  Voxel-based OIT, SIGGRAPH 2025 — [Activision](https://www.activision.com/cdn/research/AVBOIT_SIG2025_MDROBOT-final.pdf).
- No Call of Duty visibility-buffer talk was found (**UNVERIFIED** that one exists).

### Inferences
- Veach's visibility pass rasterizes ids into an `rg32_uint` color target. It needs **no 64-bit atomics**, so it
  runs on the M1. A Nanite-style compute rasterizer would need them (VCH-075) and would rule out the M1 if the forum
  claim holds.
- At 1080p the visibility buffer is 8 B/px (16.6 MB), plus 4 B/px of depth. Written once and read once per frame,
  that is about 2 GB/s at 60 fps, 3% of the M1's 68 GB/s. Bandwidth does not decide between the options; shading
  efficiency and transparency do.

### Gaps
- No primary Apple numbers for discard cost under HSR.

## 2. Apple TBDR and hardware

### Takeaway
On Apple GPUs, opaque draws must come first and alpha-tested draws after them. Tile memory makes MSAA and
single-pass deferred cheap. A visibility buffer gives up the on-tile G-buffer advantage, but its target is small.

### Cited findings
- Apple, "Tailor your apps for Apple GPUs and tile-based deferred rendering": geometry is binned per tile and only
  visible primitives are shaded. "A11 and later … improve fragment discard performance." Blending is in draw order.
  Imageblocks persist across draws and dispatches for a tile's lifetime. Tile shaders keep intermediates on-chip, a
  custom MSAA resolve can run before translucent geometry inside one pass, and raster order groups enable OIT —
  [Apple docs](https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering)
  ([JSON](https://developer.apple.com/tutorials/data/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering.json)).
- WWDC20 "Harness Apple GPUs with Metal": draw opaque first, then alpha-tested, discard and depth-feedback draws;
  never interleave them. HSR is "pixel perfect and submission order independent" —
  [WWDC20 10632](https://developer.apple.com/videos/play/wwdc2020/10632).
- Hardware:
  - M1: 8-core GPU, 2.6 TFLOPS, 68.25 GB/s — Apple newsroom, Nov 2020 (**UNVERIFIED** this session).
  - M3 Pro: 18 cores, ~150 GB/s (192-bit), down from 200 GB/s on the M1/M2 Pro —
    [MacRumors](https://www.macrumors.com/2023/10/31/apple-m3-pro-less-memory-bandwidth/); about 6.4 TFLOPS per a
    third party — [Notebookcheck](https://www.notebookcheck.net/Apple-M3-Pro-18-Core-GPU-Benchmarks-and-Specs.782445.0.html).
  - M5 Max: up to 40 GPU cores, a Neural Accelerator in each, up to 614 GB/s —
    [Apple newsroom, Mar 2026](https://www.apple.com/newsroom/2026/03/apple-debuts-m5-pro-and-m5-max-to-supercharge-the-most-demanding-pro-workflows/).
    Our machine is the 32-core bin (`system_profiler`); its bandwidth is probably lower than 614 GB/s
    (**UNVERIFIED**).
  - M3 (GPU family 9): Dynamic Caching, hardware ray tracing, mesh shading (**UNVERIFIED** this session).

### Inferences
- Single-pass deferred with a memoryless G-buffer (WWDC 2019 "Modern Rendering with Metal", **UNVERIFIED**) is
  Apple-only: DX12 desktop GPUs would write the G-buffer to memory. One path on both backends rules it out as the
  base design. It stays possible as a later Metal-specific optimization behind the same pass interface.

## 3. Anti-aliasing for foliage without upscalers

### Takeaway
No Man's Sky on Mac relies on MetalFX temporal reconstruction. Every precedent with dense foliage uses either TAA or
MSAA with alpha-to-coverage, and the visibility buffer cannot use MSAA cheaply (Engel's numbers). So a native-res TAA,
with no upscaling, is the realistic answer, and it is the same temporal machinery that clouds and GTAO want.

### Cited findings
- NMS on Mac: "new rendering pipeline … built for Metal and Apple silicon", with MetalFX upscaling —
  [nomanssky.com](https://www.nomanssky.com/2023/06/no-mans-sky-on-mac). Players report "MetalFX Temporal" and
  "MetalFX Spatial" options — [Steam](https://steamcommunity.com/app/275850/discussions/0/4526764179299716050),
  [macgamingdb](https://macgamingdb.app/games/no-man-s-sky). Developer interview —
  [Tom's Guide](https://tomsguide.com/features/exclusive-no-mans-sky-developer-shares-how-they-brought-it-to-macos-and-whats-next-for-mac-gaming).
- From memory, **UNVERIFIED**: Karis 2014 "High Quality Temporal Supersampling" (jittered TAA, YCoCg neighborhood
  clamp) — [Advances 2014](https://advances.realtimerendering.com/s2014/); Wyman & McGuire 2017 "Hashed Alpha
  Testing" (stable stochastic alpha, noisy without TAA); Castaño's coverage-preserving alpha mips; Golus "Anti-aliased
  Alpha Test" (alpha-to-coverage needs MSAA). SMAA and FXAA cannot recover subpixel foliage.

## 4. Shadows without ray tracing

### Takeaway
Cascaded shadow maps are the only option that fits today's RHI. Virtual shadow maps need indirect draws, page tables
and (in UE) Nanite. Planet-scale shadows (the planet's own night side, eclipses, rings) are analytic, not shadow maps.

### Cited findings
- **UNVERIFIED** (the Epic page returned empty): UE5 Virtual Shadow Maps use a 16k x 16k virtual resolution,
  128x128 physical pages, clipmaps for directional lights and page caching. Nanite is recommended, and world-position
  offset (wind on foliage) invalidates cached pages —
  [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine).
- **UNVERIFIED**: Valient, "Stable Cascaded Shadow Maps" (ShaderX6, 2008): texel snapping and bounding spheres stop
  shimmering. Lauritzen, Salvi & Lefohn, I3D 2011, "Sample Distribution Shadow Maps": cascades fitted to the depth
  range in view.
- Hillaire 2020: shadows cast onto the atmosphere need ray marching against shadow maps; 32 samples raise the sky to
  1.0 ms — [paper](https://sebh.github.io/publications/egsr2020.pdf).
- UE Sky Atmosphere: "Per Pixel Atmosphere Transmittance" on a directional light gives the planet's terminator and
  lets the atmosphere shadow moons —
  [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine).

### Gaps
- No sources were found on horizon-map terrain shadows or cloud shadow maps.

## 5. Indirect light without GI

### Takeaway
GTAO costs 0.5 ms at 1080p on a PS4, but only with half resolution and temporal reprojection. Sky ambient from SH or a
prefiltered sky cubemap plus probes is the industry default when there is no GI.

### Cited findings
- Jimenez et al. 2016, GTAO: "0.5 ms on a PS4 at 1080p". It computes occlusion at half resolution and upsamples. It
  distributes the integral over space and time, alternating 6 rotations with reprojection, and integrates "almost 100
  samples per pixel" —
  [paper](https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf).
- **UNVERIFIED**: Lagarde & de Rousiers 2014, "Moving Frostbite to PBR": physical light units, EV100, reflection
  probes, specular occlusion — [course notes](https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf).
  Stachowiak 2015, "Stochastic Screen-Space Reflections" — [Advances 2015](https://advances.realtimerendering.com/s2015/).

### Inferences
- Without temporal reprojection, GTAO needs about 6x the samples for the same noise. Either the temporal foundation
  comes back, or AO uses a cheaper, blurrier method (fewer directions with a wide bilateral blur).

## 6. Atmospheres from ground to orbit

### Takeaway
Hillaire 2020 already covers views from space by switching to per-pixel ray marching outside the atmosphere. It is
cheap (0.31 ms total on a GTX 1080 at 720p; 0.5 ms with the planet from space), and its LUTs rebuild in a fraction of
a millisecond. Bruneton's precompute (250 ms with the 2017 code) rules it out when parameters change per body.

### Cited findings
- Hillaire 2020 (EGSR, CGF 39(4)) — [PDF](https://sebh.github.io/publications/egsr2020.pdf),
  [DOI](https://diglib.eg.org/handle/10.1111/cgf14050):
  - Earth parameters as in [ADR 0038](../../adr/0038-physical-sky.md), with a 100 km layer. The paper lists Mie
    absorption as 4.40e-6/m. ADR 0038 says 4.40e-7: **check which one the code uses**.
  - GTX 1080: transmittance 256x64 at 0.01 ms; sky view 200x100 at 0.05 ms; aerial perspective 32³ at 0.04 ms;
    multiple scattering 32² at 0.07 ms. The sky and aerial perspective on screen cost 0.14 ms; the total is 0.31 ms
    at 1280x720.
  - iPhone 6s: transmittance 0.53 ms, sky view 96x50 (8 steps) 0.27 ms, aerial perspective 32²x16 0.11 ms,
    multiple scattering 0.12 ms. Fortnite's whole sky cost about 1 ms on that phone.
  - From space, the sky-view LUT "becomes less accurate because a large part of it is wasted to render empty space".
    The method "seamlessly switch[es] to simple ray marching on screen": the planet and atmosphere cost 0.33 ms, and
    the total is 0.5 ms.
  - The sky view is a lat/long map oriented to the camera's local up, with a quadratic mapping toward the horizon.
    The sun disk is composited afterwards.
  - Aerial perspective: 32x32 over the screen and 32 slices over 32 km by default, RGB = in-scattering,
    A = transmittance. It is applied to opaque surfaces as a post pass and per vertex for forward transparents.
  - Multiple scattering: an isotropic approximation for orders 2 and up, F_ms = 1/(1 − f_ms). Accuracy drops for high
    Mie g and in dense atmospheres. A Mars-like blue sunset is shown (Fig. 1), along with Earth and Mars from space
    (Fig. 10).
  - Bruneton-style rendering costs 0.22 ms, but its LUT update with the 2017 code costs 250 ms, 99% of it in
    multiple scattering.
- Hillaire's sample code — [GitHub](https://github.com/sebh/UnrealEngineSkyAtmosphere): the fast sky-view path only
  when `viewHeight < TopRadius`; outside, `MoveToTopAtmosphere()` advances the ray to the top sphere or rejects it.
  `AP_KM_PER_SLICE 4.0` (32 slices = 128 km in the sample). `PLANET_RADIUS_OFFSET` = 10 m.
- UE Sky Atmosphere — [docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine):
  ground-to-space transitions, with the planet placed at the origin, below the component or centered on it.
  **Two atmosphere lights**: index 0 = sun, 1 = moon. Multiple scattering is not evaluated for the second light.
  Per-pixel atmosphere transmittance gives the terminator and moon shadowing; without it a single transmittance is
  used. Reference lights: sun about 120,000 lux at 0.545°, moon 0.26 lux at 0.568°.
- Bruneton & Neyret 2008: handles ground to outer space. Five scattering orders precompute in 5 s on an 8800 GTS. The
  4D scattering table is 32x128x32x8 (8 MB in fp16) — [HAL](https://inria.hal.science/inria-00288758/document).
  The 2017 reimplementation uses the same sizes, adds ozone, custom density profiles and N-wavelength spectral
  precompute converted to sRGB, and has "no ad-hoc Earth-specific constants" —
  [ebruneton](https://ebruneton.github.io/precomputed_atmospheric_scattering/).
- Hillaire 2016, Frostbite: Rayleigh from refractive index, σ = 8π³(n²−1)²/(3Nλ⁴)·(6+3p_n)/(6−7p_n). With
  n = 1.0003, N = 2.545e25 m⁻³ and p_n = 0.035 this gives (5.47e-6, 1.28e-5, 3.12e-5)/m, which is the route for other
  gas mixtures. Ozone comes from cross-section × number density. On Xbox One: main view 0.42 ms at 720p; AP volume
  0.05 ms; LUT update 3.5 ms at once, or 0.22 ms/frame spread over 19 frames. Clouds 0.91 ms at 720p, 1.60 ms at
  1080p — [course notes](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf).
- SpaceEngine replaced O'Neil's atmosphere with one based on Bruneton's (2010 news) —
  [spaceengine.org](https://spaceengine.org/news/blog100301/). No later details.

### Inferences
- **Scale law** (derivation): optical depth τ = ∫σ(h)ds. If every planet length (radius, top, scale heights) is
  multiplied by s, coefficients multiplied by 1/s keep τ along any path that scales with the planet. The sky from the
  ground and the planet from orbit then look the same at any s. A path whose length does **not** scale (1 km to a
  house) gets 1/s the optical depth it should: haze is 10x thicker at s = 0.1.
- Composition-driven coefficients: Rayleigh from (n, N) per gas via the Frostbite formula. Mie and absorbers from
  aerosol and absorber tables (Q3 owns the tables).

### Gaps
- No sources were found on alien-atmosphere spectral methods (Titan haze, Mars dust) in this session.
- Kerbin's figures (radius 600 km, atmosphere 70 km, Earth gravity) are from memory: the KSP wiki blocked automated
  access (**UNVERIFIED**).

## 7. Shipped planet renderers and depth precision

### Cited findings
- Outerra (Kemen), log depth — [2012 post](https://outerra.blogspot.com/2012/11/maximizing-depth-buffer-range-and.html),
  [2009 post](https://gamedev.net/blogs/entry/2001520-logarithmic-depth-buffer):
  - A standard depth buffer is usable for about 4 decades; log depth covers 9.
  - A reversed 32-bit float buffer is slightly better than 24-bit log; a 32-bit log buffer is about 20x finer.
  - Log depth is correct only at vertices, so large triangles need fragment depth, which disables early-z
    (8 to 10% fps when objects cover the screen). The conservative-depth hint gave no speedup.
- Cesium — [blog 2018](https://cesium.com/blog/2018/05/24/logarithmic-depth/): it used to render 3 frustums
  (1 m to 1 km, 1 km to 1000 km, 1000 km to 1e6 km) back to front, clearing depth between them. Objects that
  straddled a boundary were drawn twice (26 of 137 draws). Log depth (near 0.1, far 1e8) brought that to 111 draws,
  but needs fragment depth, so early-z is lost. Planes 300 m apart still z-fight from about 64,000 km.
- Reed 2015, "Depth Precision Visualized" — [reedbeta](https://www.reedbeta.com/blog/depth-precision-visualized/):
  with near 0.1 and far 10K, standard depth gives 45% indistinguishable / 18% swapped. Reversed-Z float32 gives 0% /
  0%, with a finite or infinite far plane. An infinite far plane has negligible precision cost. Keep view and
  projection separate.
- Unity HDRP camera-relative rendering is on by default: positions are offset by −cameraPos before transforms —
  [Unity](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/Camera-Relative-Rendering.html).
- UE5 LWC: translated (camera-relative) world space is "safe to use floats"; DoubleFloat in shaders has a
  "substantial" cost. The tile size is unclear (256k in the docs, 2097152 per a forum thread) —
  [UE docs](https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5?lang=en-US).
- KSP ScaledSpace is the planetarium coordinate system, the world scaled down by `ScaleFactor` —
  [API docs](https://anatid.github.io/XML-Documentation-for-the-KSP-API/class_scaled_space.html). The value 6000 and
  the separate camera layers are **UNVERIFIED** (forum only).
- Elite Dangerous: Stellar Forge, about 400 billion systems, roughly 160,000 from catalogs —
  [Space.com](https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html). The skybox shows sector seams
  where star density changes — [Frontier forum](https://forums.frontier.co.uk/threads/borders-between-codex-regions-seem-to-also-be-borders-between-different-background-star-fields.616573).
- Star Citizen Planet Tech v4/v5 — [starcitizen.tools](https://starcitizen.tools/Planet_Tech_V5): press-level only.
  The Gen12 atmosphere and cloud ports are mentioned in
  [Monthly Report June 2022](https://starcitizen.tools/Comm-Link:Star_Citizen_Monthly_Report_-_June_2022).
  No technical detail (**UNVERIFIED**).
- Outer Wilds: only the art director's "Simulating a Planet's Atmosphere" video (layered atmospheres, "No skybox
  here!"), **UNVERIFIED**.
- NMS: "Building Worlds Using Math(s)", GDC 2017 — [GDC Vault](https://gdcvault.com/play/1024514/). No rendering
  details found.

### Inferences (depth)
- Reverse-Z float32 relative precision is about 2^-23 of distance: about 0.01 m at 100 km, 1.2 m at 10,000 km and
  12 m at 1e8 m. That is enough for one surface at a time at any distance. It is **not** enough for a 1 m object
  standing on a planet surface 10,000 km away, which does not matter because such an object is subpixel.
- Log depth is wrong for Apple TBDR: fragment depth writes disable HSR and early-z on exactly the GPUs we target.

## 8. Rings, gas giants, starfields, eclipses

### Cited findings
- Gas giants: Cameron, "Gaseous Giganticus", curl-noise particle advection over band velocity fields (offline) —
  [slides](https://smcameron.github.io/space-nerds-in-space/gaseous-giganticus-slides/slideshow.html). Curl-noise flow
  maps on a sphere in compute, one pass per octave — [jasper-r](https://jasper-r.github.io/gas-giant). GPU noise
  tutorial — [moddb](https://www.moddb.com/news/procedural-gas-giant-rendering-with-gpu-noise).
- Star brightness: m = −2.5·log10(E/E₀). Wikipedia's data points imply E₀ ≈ 2 to 2.5e-6 lux for magnitude 0
  (−14.2 ≈ 1 lux; full Moon −12.6 ≈ 0.267 lux). The whole night sky with airglow is about 0.002 lux —
  [Wikipedia](https://en.wikipedia.org/wiki/Apparent_magnitude) (derivation; the zero point varies by source).
- Ring shadows, analytic eclipses, nebula volumes and the impostor handoff were not researched before the quota ran
  out. Their designs in the deliverable are derived from geometry, not from a precedent (**UNVERIFIED** as shipped
  practice).

## 9. Volumetric clouds

### Takeaway
Every shipped volumetric cloud system depends on temporal reprojection or accumulation. Horizon Zero Dawn costs about
2 ms on a PS4 with reprojection and about 20 ms without it. Clouds are therefore the feature that decides whether
Veach brings back a temporal foundation.

### Cited findings
- Schneider, SIGGRAPH 2015, Horizon Zero Dawn —
  [slides](https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf):
  - Budget and memory: about 2 ms on PS4, 20 MB.
  - Noise: base 128³ Perlin-Worley, detail 32³, 2D curl noise 128².
  - Weather texture RGB = coverage, precipitation, type.
  - Layer and draw range: volumetric clouds from 1,500 to 4,000 m; 2D textures above 4,000 m; drawn within 35 km on a
    curved shell.
  - Sampling: 64 to 128 view samples, 6 cone light samples.
  - **Without reprojection about 20 ms.** Reprojection updates 1 of 16 pixels per 4x4 block of a quarter-res buffer
    per frame: "pretty much the whole reason we are able to put this in our game".
- Nubis, SIGGRAPH 2017 — [slides](https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf):
  PS4 cost chain with reprojection: 22 → 8.1 (adaptive step and LOD) → 3.34 (light samples only where density is
  nonzero) → 1.81 (horizon cull) → 1.2 ms (depth cull). "The reason we are able to render our cloudscapes in 2 ms is
  re-projection."
- Nubis Evolved, SIGGRAPH 2022 — [slides](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf):
  PS4 traces at up to 960x540 with 6 light and 60 to 90 view samples, at 4 ms or less. PS5 traces at up to 1080p
  with 10 light and 96 to 180 view samples, at 2 to 3 ms or less.
- Nubis Cubed, SIGGRAPH 2023 — [slides](https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf):
  128³ voxel cloud grids, compressed with an SDF, at 2.2 to 4 ms. Light density is cached every 8 frames, which cuts
  render time by 40%.
- Hillaire 2016 (Frostbite), Xbox One: clouds 0.91 ms at 720p and 1.60 ms at 1080p, at half resolution. Jittered
  samples are blended with the reprojected previous frame (an EMA) —
  [course notes](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf).
- UE Volumetric Cloud: mode 0 traces at quarter resolution with temporal reconstruction, and Epic recommends it for
  ground-to-space views. Ray-marched shadows are recommended for ground-to-space transitions —
  [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine).
  Users report temporal artifacts from the reconstruction —
  [forum](https://forums.unrealengine.com/t/volumetric-cloud-temporal-artifacts-from-volumetric-render-target-reconstruction/2649937).
- Toft et al. 2016, with no temporal reuse: 297.7 ms (full res, 128 steps), 128.0 ms (half res, 128 steps), 2.3 ms
  (half res, 8 steps); the 8-step result needs TAA to clean up — [arXiv](https://arxiv.org/pdf/1609.05344).
- Clouds seen from orbit: no primary source for Star Citizen, Elite Odyssey or MSFS (**UNVERIFIED**). Academic leads:
  [TU Wien poster](https://cg.tuwien.ac.at/research/publications/2019/kerbl_2019_planet_poster),
  [Pixar](https://graphics.pixar.com/library/PlanetAndClouds/paper.pdf).

### Inferences
- PS4 is 1.84 TFLOPS with 176 GB/s; the M1 has 2.6 TFLOPS but 68 GB/s (TBDR saves bandwidth on render targets, not
  on 3D noise fetches). A PS4-class 2 to 4 ms budget is a reasonable **estimate** for M1 clouds at 1080p, and only
  with reprojection.
- HZD's cloud layer (1.5 to 4 km above the ground) does not fit inside an atmosphere scaled linearly by s = 0.01
  (a 1 km-high atmosphere). Cloud heights cannot simply follow s at small s.

## 10. Oceans and underwater

### Cited findings
- Tessendorf, "Simulating Ocean Water" (2004 revision): FFT patches from 10 m to 2 km, up to 2048², visible tiling
  unless the patch is large, looping dispersion —
  [course notes](https://people.computing.clemson.edu/~jtessen/reports/papers_files/coursenotes2004.pdf).
- Atlas, GDC 2019 (Grapeshot and NVIDIA): 4 frequency bands of small FFTs recombined in the shader, post-processed
  into a Beckmann BRDF. Wind waves at max quality cost 0.5 ms on a Radeon VII or RTX 2080; wakes and explosions cost
  0.6 to 0.8 ms — [slides](https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd6-interactive-water-simulation-in-atlas.pdf).
- Crest: concentric power-of-2 LOD rings whose scale follows viewer height. Each wave component renders into the LOD
  matching its wavelength — [system notes](https://crest.readthedocs.io/en/4.23.0/user/system-notes.html),
  [waves](https://crest.readthedocs.io/en/4.23.0/user/waves.html).
- Bruneton, Neyret & Holzschuch 2010: ocean geometry is filtered into normals, then into a BRDF, as distance grows.
  This gives seamless ground-to-orbit ocean lighting — [INRIA](https://evasion.inrialpes.fr/Publications/2010/BNH10).
- Snell's window: critical angle arcsin(1/1.333) ≈ 48.6°, a cone about 97° wide (physics, no game source).
- Sea of Thieves, AC IV, underwater fog and caustics: not reached (**UNVERIFIED**).

## 11. Exposure

### Cited findings
- Frostbite, "Moving Frostbite to PBR" —
  [course notes](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v32.pdf):
  - Histogram auto-exposure, because average log luminance is unstable with flickering lights.
  - EV100 to luminance (K = 12.5): EV −4 = 0.008 cd/m², EV 0 = 0.125, EV 16 = 8192.
  - Sun luminance is about 1.6e9 cd/m². Sun illuminance at the surface is 105,000 to 114,000 lux.
  - **Pre-exposure**, and a scene dynamic range of 100,000:1.
- UE auto-exposure: a 64-bin histogram by default; percentiles low 70 to 80% and high 80 to 95%; adaptation speeds in
  f-stops per second; linear to exponential adaptation within 1.5 stops —
  [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/auto-exposure-in-unreal-engine).
- Narkowicz 2016: 64 bins cover 16 EV and 128 bins cover the real-world range. Discard the darkest 50 to 80% and the
  brightest 2 to 20%. Adaptation L += (L_target − L)(1 − e^(−Δt·τ)) with separate up and down speeds. Real eyes take
  5 min to light and 20 to 30 min to dark, too slow for games. Key curve 1.03 − 2/(log10(L+1)+2) —
  [blog](https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/).
- Illuminance — [Wikipedia: Lux](https://en.wikipedia.org/wiki/Lux):

  | Condition | Lux |
  |---|---|
  | Overcast moonless night (starlight) | 0.0001 |
  | Clear moonless night | 0.002 |
  | Full moon | 0.05 to 0.3 |
  | Overcast day | 1,000 |
  | Direct sun | 32,000 to 100,000 |

  Starlight to direct sun is about 1e9, roughly 30 EV.

### Inferences
- The HDR target is `rgba16_float` today (max 65,504). The sun disk at about 1.6e9 nits overflows it unless the
  shade pass pre-exposes (multiplies by the previous frame's exposure) as Frostbite does. Pre-exposure is a
  prerequisite for auto-exposure, and probably already a latent bug for the sun disk (**UNVERIFIED** what the shade
  pass does today).

## 12. Vegetation at scale

### Cited findings
- Horizon Zero Dawn GPU placement (GDC 2017): density map → GPU generation → a point cloud of 3x4 matrices, up to 64
  pipelines in flight, footprints 0.5 to 2 m, about 250 µs average busy load —
  [slides](https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf).
- Assassin's Creed Unity (SIGGRAPH 2015): about 10x the instances of earlier games because the CPU was the scarce
  resource. Clusters of 64 triangles, GPU culling per instance, then per cluster (frustum and occlusion), then per
  triangle (backface). The previous frame's depth is reprojected in 50 µs on PS4 —
  [slides](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf).
- Ghost of Tsushima grass (GDC 2021): every blade is generated on the GPU —
  [GDC Vault](https://www.gdcvault.com/play/1027214/Advanced-Graphics-Summit-Procedural-Grass). Counts, ms and the
  AA approach are **UNVERIFIED**.

## 13. No Man's Sky specifics

- Worlds Part I (v5.0, July 2024): the "atmospheric and volumetric cloud rendering system has been completely
  rewritten", with volumetric fog, cirrus and nimbus clouds, and storms —
  [nomanssky.com](https://nomanssky.com/worlds-part-i-update).
- Steam minimum: GTX 1060 3GB / RX 470 / UHD 630 on Windows; Radeon Pro 570X on macOS 12.3 —
  [Steam](https://store.steampowered.com/app/275850/No_Mans_Sky/).
- No technical talk on NMS rendering, grading or clouds was found (**UNVERIFIED** that one exists).

## 14. Apple GPUs for scaling

| Chip | GPU cores | FP32 | Bandwidth | Source |
|---|---|---|---|---|
| M1 | 8 | 2.6 TFLOPS | 68 GB/s | [Apple 2020](https://www.apple.com/newsroom/2020/11/apple-unleashes-m1/), [Wikipedia](https://en.wikipedia.org/wiki/Apple_M3) |
| M3 Pro | 18 (or 14) | ~6.4 TFLOPS (third party) | 150 GB/s | [Apple 2023](https://www.apple.com/newsroom/2023/10/apple-unveils-m3-m3-pro-and-m3-max-the-most-advanced-chips-for-a-personal-computer/), [MacRumors](https://www.macrumors.com/2023/10/31/apple-m3-pro-less-memory-bandwidth/) |
| M5 Max (ours) | 32 | not published | 460 GB/s | [Apple MacBook Pro specs](https://www.apple.com/macbook-pro/specs/) |
| M5 Max | 40 | not published | 614 GB/s | same |

- Apple claims M3 graphics are up to 65% faster than M1 (same newsroom).
- No cross-chip benchmark (3DMark, GFXBench) was retrieved (**UNVERIFIED**).

### Inferences (estimates, not measurements)
- FP32 by cores × 128 lanes × 2 × clock: M1 at 1.28 GHz gives 2.6 TFLOPS (checks against Apple). Our 32-core M5 Max
  at an assumed ~1.6 GHz gives about 13 TFLOPS (**UNVERIFIED** clock). Ratios to the M1: compute ≈ 5x, bandwidth ≈
  6.8x. M3 Pro to M1: compute ≈ 2.5x, bandwidth ≈ 2.2x.
- Working scaling factors for budgets: **M1 time ≈ 6 × M5 Max time (range 5 to 8); M3 Pro time ≈ 2.5 × M5 Max time
  (range 2 to 3)**. These are estimates, to be replaced by one measurement on each class of machine.

## 15. RHI facts found in the code (not in the audit)

- `GraphicsPipelineDesc` has no blend state, no MSAA sample count, no depth bias. `SamplerDesc` has only
  filter (nearest/linear) and address (clamp/repeat): no comparison samplers, no anisotropy, no mirror or border.
  `TextureDesc` has no depth, layers or sample count. Formats lack r8, rg8, r16f, rg16f, r11g11b10f and depth16.
  (`engine/rhi/include/veach/rhi/types.hpp`). Forward transparency needs blending; shadow maps want comparison
  sampling and depth bias (or manual PCF in shader); TAA and motion vectors want rg16f.
- The sky's Mie absorption: the code uses 4.40e-7/m (`engine/render/src/atmosphere.cpp`, 4.40e-4 per km), matching
  ADR 0038. Hillaire's paper Table 1 prints σma = 4.40e-6/m. Hillaire's sample code (from memory, **UNVERIFIED**)
  has Mie extinction 4.40e-3/km and scattering 3.996e-3/km, which gives absorption 4.4e-7/m. The paper's table
  probably prints extinction in the absorption column. This is low impact; check once against the sample code.
- Hillaire 2020 integrates over "a set of N_light directional lights, e.g. a sun and a moon" (paper text, section 4).
