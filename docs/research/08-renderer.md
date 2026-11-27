# 08: The real-time renderer and astronomical-scale render features

Question 8 of the research phase ([VCH-070](../backlog/BACKLOG.md)): which base renderer the real-time path should be
(forward+/clustered, deferred, or the visibility buffer we have), and how planet- and space-scale features plug in as
`RenderFeature`s and stay correct as radius and atmosphere height change by orders of magnitude, at every creator
scale s. It respects the (a) items that [00-scope.md](00-scope.md) assigns to Q8: N suns, an atmosphere per body seen
from orbit, auto-exposure, impostors and the far-body handoff, 3D textures and indirect draw as prerequisites, and a
human-scale fog control separate from s.

Raw notes, measurements and every source: [notes/08-renderer-notes.md](notes/08-renderer-notes.md). **Estimate**
marks a number that was not measured. **Unverified** marks a claim that was not checked against a primary source.

## 1. The problem in Veach's terms

The engine has the parts a renderer needs but not the renderer: a render graph that derives barriers and residency
([ADR 0029](../adr/0029-render-graph-sync-and-barriers.md)), bindless Slang ([0027](../adr/0027-resource-binding-model.md)),
Slang materials compiled into a static dispatch ([0035](../adr/0035-material-model.md)), photometric lights
([0036](../adr/0036-lights-in-photometric-units.md)), EV100 exposure and AgX ([0037](../adr/0037-exposure-and-agx.md)),
Hillaire sky LUTs ([0038](../adr/0038-physical-sky.md)), and an interim visibility-buffer path
([0040](../adr/0040-interim-realtime-path-visibility-buffer.md)). That path lights every pixel with every light,
unshadowed, under a 1x1 sky-ambient texel, with no transparency, AA, AO or reflections.

**What it costs today, measured on the M5 Max (32-core GPU), Metal, render-graph timestamps:**

| Scene | Resolution | visibility | sky LUTs | shade | display | frame |
|---|---|---|---|---|---|---|
| Bistro exterior | 1080p | 5.7 to 7.0 ms | 0.17 to 0.25 | 0.73 to 0.80 | 0.1 to 0.2 | 6.9 to 8.1 |
| Bistro exterior | 1440p | 3.8 to 7.2 | 0.13 to 0.24 | 1.03 to 1.37 | 0.15 to 0.19 | 5.2 to 9.0 |
| Sponza | 1080p | 1.07 | 0.06 | 0.83 | 0.05 | 2.0 |
| Sponza | 1440p | 1.18 | 0.06 | 1.46 | 0.08 | 2.8 |

The visibility pass draws every meshlet of every instance with no culling of any kind, so Bistro is geometry-bound and
noisy. The per-pixel work is small. The sky LUTs cost almost nothing.

**What the toolbox asks of the renderer** (from the brief and the scope's section 10):

- A stylized-but-PBR look in the spirit of No Man's Sky, with no real-time GI or ray tracing ([0039](../adr/0039-realtime-renderer-is-raster-pbr.md)).
- Terrain, dense alpha-tested instanced vegetation, many small lights (bases, ships, creatures).
- Transparency: water, glass, clouds, particles.
- Planets seen from the ground, from orbit and from other bodies, with atmospheres driven by composition, or none.
- Several suns, eclipses, rings, gas giants, oceans and underwater, and a starfield and deep sky generated from the galaxy.
- Exposure from deep space (stars around 1e-6 lux each) through sunlit ground (100,000 lux) to caves.
- Depth from a 0.05 m cockpit near plane to a gas giant 1e8 m away.
- All of it at any creator scale s, on a base M1 and an M3 Pro-class GPU, verified headless on the M5 Max.

**The flat-world assumptions Q8 must remove** (dev-lead inventory): sky altitude = world Y, zenith = +Y, the sun from
Earth astronomy, a fixed Earth radius, one `Sky` per scene, LUTs that break above the atmosphere top, one directional
sun, a fixed EV100. Two more turned up in this research:

- `FrameContext` hands features only an `rgba8_unorm` `scene_color`. The HDR target, depth and visibility buffer are
  private to `SceneRenderer`, so no module can add a lit, depth-tested feature today.
- The HDR target is `rgba16_float` (max 65,504), while the sun's disk is about 1.6e9 nits
  ([Frostbite](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v32.pdf)).
  Without pre-exposure the disk overflows. That is unverified as a visible bug today, but certain once auto-exposure
  lets the camera look at the sun from space.

**RHI gaps found in the code that the audit doesn't list:** no blend state, no MSAA, no depth bias, no comparison
samplers, no anisotropic filtering, no r8/rg8/r16f/rg16f/r11g11b10f formats (`engine/rhi/include/veach/rhi/types.hpp`).
Forward transparency cannot be drawn without blending.

## 2. Options, with precedents

### 2.1 The base renderer

| Option | What it is | Precedent |
|---|---|---|
| **A. Visibility buffer (keep and grow ADR 0040)** | Rasterize ids, then shade each pixel once in compute. Add GPU culling, material binning, clustered light lists, and a forward pass for transparents. | Burns & Hunt 2013 ([JCGT](https://jcgt.org/published/0002/02/04/paper.pdf)); ConfettiFX/The Forge ([GDC 2016](https://www.slideshare.net/slideshow/triangle-visibility-buffer/238860894), [v1.57](https://github.com/ConfettiFX/The-Forge/releases/tag/v1.57)); Nanite ([SIGGRAPH 2021](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)) |
| **B. Clustered forward (forward+)** | A depth prepass, then shade in the fragment shader against a clustered light list. MSAA is natural. | id Tech Doom 2016/Eternal ([study](https://adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study), [SIGGRAPH 2020](https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf)); Olsson et al. 2012 ([preprint](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf)); Call of Duty z-binning ([Drobot 2017](https://www.advances.realtimerendering.com/s2017/2017_Sig_Improved_Culling_final.pdf)) |
| **C. Deferred (G-buffer)** | Write surface attributes, then light them. On Apple GPUs the G-buffer can stay in tile memory (single-pass deferred). | Frostbite, UE; Apple's tile-shading samples (WWDC 2019, unverified) |

Facts that decide it:

- **Bandwidth.** The visibility buffer beat a G-buffer at every resolution in Engel's measurements: 8.57 vs 9.75 ms at
  1080p and 15.19 vs 20.19 at 4K on an R9 380. The gap widens to 37.86 vs 69.64 ms with 4x MSAA. Ours is 8 bytes per
  pixel, about 2 GB/s at 1080p60, which is 3% of an M1's 68 GB/s. Bandwidth is not the problem for option A.
- **Vegetation.** On Apple's TBDR GPUs, alpha-tested draws defeat hidden-surface removal for what they cover. Apple
  says to draw opaque geometry first, then alpha-tested, and never interleave them
  ([WWDC20](https://developer.apple.com/videos/play/wwdc2020/10632)).
  - In option B every overlapping foliage layer runs the **full** material and lighting shader, because coverage
    is known only after the shader runs.
  - In option A the masked fragment does one alpha fetch and writes 8 bytes, and the pixel is lit once.
  - For a planet carpeted in grass and trees this is the deciding argument.
- **Transparency** is forward in every option. The Forge sorts transparents back to front; Nanite supports only
  opaque and masked. So option A is really "visibility buffer for opaque and masked, clustered forward for
  transparents", and the clustered light grid is shared.
- **MSAA** is where option B wins. A visibility buffer under MSAA must shade per sample at the edges (Engel: the
  shade stage rose from 3.43 to 15.58 ms with 4x MSAA at 4K). Foliage AA then has to come from TAA (section 2.2).
- **Material divergence.** One compute shade pass over many material kinds diverges. Nanite moved from per-material
  full-screen passes to compute "shading bins" (one dispatch per bin; [notes](https://scthe-github-io.vercel.app/blog/nanite-materials-notes)).
  Veach's `evaluateSurface` switch is the same model at a smaller size. Binning needs indirect dispatch (VCH-071).
- **Culling** is the first win whichever option we pick. Wihlidal culled 78% of triangles by cone, frustum, Hi-Z and
  small-primitive tests ([GDC 2016](https://gdcvault.com/play/1023109/Optimizing-the-Graphics-Pipeline-With)). AC
  Unity culled per instance, then per cluster, then per triangle
  ([SIGGRAPH 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)).
  It needs indirect draw (VCH-071).
- **The M1 and 64-bit atomics.** Nanite's compute rasterizer writes ids with 64-bit atomics. A forum report says
  Metal has them only from Apple8 (M2) (unverified). Our visibility pass rasterizes into an `rg32_uint` color target
  and needs no atomics, so it runs on the M1. A software rasterizer is out while the M1 is in the target.
- **Option C on Apple** keeps the G-buffer on tile, which is elegant. On DX12 desktop GPUs, though, the G-buffer goes
  to memory, and we want one path that both backends verify. It remains a later Metal-only optimization behind the
  same interfaces.

### 2.2 Anti-aliasing (the reset removed the temporal upscalers)

| Option | Fits | Cost |
|---|---|---|
| None, plus specular-aliasing filtering (Toksvig-style roughness) | Option A | Free. Foliage shimmers and sparkles at distance |
| FXAA/SMAA | A or B | Cheap. Cannot recover subpixel foliage (unverified common knowledge) |
| MSAA + alpha-to-coverage | B only | Cheap on TBDR (on-tile resolve). Needs MSAA in the RHI. Does nothing for shader aliasing |
| **Native-resolution TAA** (jitter, motion vectors, history, neighborhood clamp; Karis 2014, unverified) | A or B | ~0.3 to 0.5 ms (estimate). Ghosting must be tuned. Brings back the temporal machinery the reset deleted |

No Man's Sky on Mac ships MetalFX temporal reconstruction ([nomanssky.com](https://www.nomanssky.com/2023/06/no-mans-sky-on-mac),
[Steam](https://steamcommunity.com/app/275850/discussions/0/4526764179299716050)). **TAA is needed for vegetation, and
for more than vegetation:**

- Volumetric clouds cost about 20 ms on a PS4 without reprojection and about 2 ms with it. Every shipped cloud system
  depends on it: HZD, Nubis, Frostbite, UE.
- GTAO's 0.5 ms comes from temporal reuse of 6 rotations ([paper](https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf)).

One native-resolution temporal foundation serves all three. Upscaling stays out.

### 2.3 Shadows without ray tracing

| Option | Notes |
|---|---|
| **Cascaded shadow maps (CSM)**, 3 to 4 stable, texel-snapped cascades, camera-relative (Valient 2008; SDSM, Lauritzen 2011, unverified) | Fits today's RHI apart from depth bias and comparison samplers (manual PCF works without them). Covers meters to a few km. |
| Virtual shadow maps (UE5: 16k virtual resolution, 128² pages, clipmaps; unverified) | Needs indirect draw, page tables and feedback. Nanite recommended; wind-animated foliage invalidates cached pages. Too much for the M1 and for VCH-071's first scope. |
| **Analytic occluders** for anything planet-sized: the planet's own night side (transmittance LUT, Hillaire), eclipses (sun-disk overlap by spheres), ring shadows (ray-plane against the ring's radial transmittance) | Exact, cheap, scale-free. UE uses per-pixel atmosphere transmittance for the terminator and for moons ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine)). |
| Cloud shadows | A top-down transmittance map from the cloud layer, sampled like a light cookie (no source found; standard practice, unverified). |
| Far terrain self-shadowing (mountains at 50 km) | The last cascade at low resolution, or a horizon map from Q4's coarse height data. Defer. |

### 2.4 Indirect light without GI, and reflections

- **Sky ambient as L2 spherical harmonics**, projected from the sky-view LUT every frame. This replaces the 1x1
  upward-irradiance texel, which is wrong for walls and for any normal on a curved planet.
- **GTAO** at half resolution with temporal reuse: 0.5 ms at 1080p on a PS4
  ([Jimenez 2016](https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf)).
- **Specular:** a prefiltered sky cubemap rebuilt from the sky LUT (needs cube maps, VCH-072), plus local reflection
  probes captured at bases and caves (Frostbite PBR course,
  [notes](https://seblagarde.files.wordpress.com/2015/07/course_notes_moving_frostbite_to_pbr_v32.pdf), unverified
  detail). Probes are relit by sky changes only approximately.
- **SSR** (Stachowiak 2015, unverified): an M3 Pro-tier option for water and wet surfaces, not baseline.
- **Planetshine and moonlight:** a dim directional light per lit nearby body, computed from its albedo and phase
  (the scope's (b) item). It is data for the clustered or directional light list.

### 2.5 Atmospheres from ground to orbit, per body

| Option | Cost | Fit |
|---|---|---|
| **Hillaire 2020 LUTs, generalized** (center and radius per body, local up, per-pixel ray march from space) | GTX 1080: whole sky 0.31 ms at 720p, 0.5 ms with the planet from space. LUT rebuild under 0.2 ms. Ours today: 0.06 to 0.25 ms on the M5 Max | Already ours. The paper and UE both do space views by switching to an on-screen ray march outside the atmosphere ([paper](https://sebh.github.io/publications/egsr2020.pdf), [sample code](https://github.com/sebh/UnrealEngineSkyAtmosphere)). Loses accuracy in dense atmospheres and at high Mie g. |
| Bruneton 2008/2017 precomputed 4D scattering | Render 0.22 ms; precompute **250 ms** with the 2017 code, 99% of it multiple scattering ([Hillaire 2020](https://sebh.github.io/publications/egsr2020.pdf), [Bruneton](https://ebruneton.github.io/precomputed_atmospheric_scattering/)) | More accurate multiple scattering and spectral support. Too slow to rebuild when bodies change, and 8 MB per body. |
| Full per-pixel ray march everywhere | High (Hillaire's motivation for the LUTs) | Only for distant bodies, which cover few pixels. |

**Composition drives the coefficients, not the technique.**

- Rayleigh comes from each gas's refractive index and number density: σ = 8π³(n²−1)²/(3Nλ⁴)·(6+3p_n)/(6−7p_n)
  ([Frostbite 2016](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf)).
- Ozone-like absorbers come from cross-section × density, and Mie from an aerosol table.
- Q3 owns the tables. Q8 consumes an `AtmosphereRecord`: radius, top, a density profile per component, coefficients,
  phase g and ground albedo.
- Hillaire already reproduces a Mars-like blue sunset. For very dense atmospheres (Venus, Titan) the
  multiple-scattering approximation drifts. That is acceptable for a stylized look and a known limit.

**Multiple suns.** Hillaire's integral already takes N directional lights. UE supports two lights and skips multiple
scattering for the second. For Veach:

- The transmittance and multiple-scattering LUTs are per body and shared by all suns.
- The sky-view LUT is parameterized in **absolute azimuth around local up**, with all suns summed into one table.
  This gives up the sun-relative symmetry trick for one table.
- The aerial-perspective volume sums all suns as it marches.
- The extra cost per sun is one more phase and transmittance evaluation per sample, not another LUT pass.

### 2.6 Volumetric clouds

| Option | Cost | Notes |
|---|---|---|
| **Nubis-style shell ray march** with a per-body weather map, 3D noise and quarter-res reprojection ([HZD 2015](https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf), [Nubis 2017](https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf), [Evolved 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf)) | PS4 about 2 ms with reprojection, 20 ms without; PS4 at 960x540 up to 4 ms; PS5 2 to 3 ms | Needs 3D textures (VCH-072) and the temporal foundation. NMS rewrote its clouds this way in 2024 (volumetric clouds and fog, [Worlds Part I](https://nomanssky.com/worlds-part-i-update)). |
| Voxel clouds (Nubis Cubed, 128³ grids with an SDF) | 2.2 to 4 ms | For hero clouds and storms; later. |
| 2D layers (scrolling textures on a shell) | ~0.2 ms (estimate) | The M1 low tier, high cirrus (HZD does this above 4 km), and every planet seen from orbit. |
| No temporal reuse, few steps | Toft et al.: half res, 8 steps, 2.3 ms, noisy until TAA ([arXiv](https://arxiv.org/pdf/1609.05344)) | Not viable alone. |

From orbit, clouds are the weather map's coverage drawn on the body's analytic shell, shaded with the atmosphere's
transmittance. The ray march runs only near and inside the layer. UE recommends quarter-res tracing and ray-marched
shadows for ground-to-space ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine)).

### 2.7 Oceans and underwater

- **Surface:** wave bands from small FFTs recombined in the shader (Atlas: 4 bands, 0.5 ms on an RTX 2080,
  [GDC 2019](https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd6-interactive-water-simulation-in-atlas.pdf);
  Tessendorf, [notes](https://people.computing.clemson.edu/~jtessen/reports/papers_files/coursenotes2004.pdf)).
- **Geometry:** Crest-style concentric LOD rings whose scale follows camera height
  ([Crest](https://crest.readthedocs.io/en/4.23.0/user/system-notes.html)), on a **sea-level sphere** per body.
  Vertices are camera-relative and displaced along the local normal.
- **From altitude:** geometry detail filters into normals, then into a BRDF (Bruneton, Neyret & Holzschuch 2010,
  [INRIA](https://evasion.inrialpes.fr/Publications/2010/BNH10)). The ocean seen from orbit is the same shader
  term on the analytic impostor.
- **Underwater** is a **camera medium**, not a special case:
  - the frame knows whether the camera is in air (of body X), water, vacuum or a gas giant's deep layer;
  - extraction resolves it, and the fog, absorption and refraction features read it;
  - Snell's window is a cone of about 97° (critical angle 48.6°).
  - Caustics are a later module.

### 2.8 Rings, gas giants, eclipses, the starfield, deep sky, distant bodies

No precedent was verified for these in this session (the search quota ran out). The designs below are derived from
geometry and marked as such.

- **Rings** (derived): a flat annulus drawn analytically, with a 1D radial texture (optical depth, albedo, color)
  from Q3, forward-scattering phase, and the planet's shadow by ray-sphere. Their shadow on the planet is a ray-plane
  intersection per pixel into the same 1D texture. Inside a ring, asteroid instancing is a separate (b) module.
- **Gas giants:** an analytic sphere (no terrain), banded albedo from curl-noise flow advected in compute
  ([Gaseous Giganticus](https://smcameron.github.io/space-nerds-in-space/gaseous-giganticus-slides/slideshow.html),
  [jasper-r](https://jasper-r.github.io/gas-giant)), and the generic atmosphere feature on top with a large scale
  height. Diving in, the camera medium switches to "deep atmosphere" fog. The band texture updates incrementally,
  needing texture updates (VCH-076) or a compute-written storage texture.
- **Eclipses** (derived): Q2 answers "which bodies occlude sun S from point P at tick t". Extraction turns that into a
  per-sun **visible fraction** (the overlap area of two disks) for the local scene. A short analytic occluder list
  (spheres, rings) lets far bodies and the atmosphere ray march shade per pixel. It is deterministic, computed on the
  CPU from Q2's state, and costs nothing on the GPU for the common case.
- **Starfield:** Q3's galaxy query returns near stars (direction, distance, absolute magnitude, temperature).
  - Each star draws as an energy-conserving point (a small PSF whose integral is the star's illuminance,
    E = E₀·10^(−0.4m) with E₀ ≈ 2 to 2.5e-6 lux, [derivation](https://en.wikipedia.org/wiki/Apparent_magnitude)),
    so exposure and bloom treat stars physically.
  - The rest of the galaxy (the disc, dust lanes, the core, far clusters, nebulae seen from outside) is baked into a
    background cubemap by a compute pass when the camera changes system. Elite's sector-seam artifacts
    ([forum](https://forums.frontier.co.uk/threads/borders-between-codex-regions-seem-to-also-be-borders-between-different-background-star-fields.616573))
    show why the bake must be continuous across sector boundaries.
  - Nebulae you fly inside are 3D-texture volumes (VCH-072), a (b) module.
- **Distant bodies and the terrain handoff** (derived, after KSP's ScaledSpace idea,
  [API](https://anatid.github.io/XML-Documentation-for-the-KSP-API/class_scaled_space.html)), in three tiers chosen
  by **projected size in pixels**, which is scale-free:
  1. **Point.** Under about 2 px, a point with the body's phase-dependent illuminance.
  2. **Analytic impostor.** A screen-space quad that ray-intersects the sphere. Its albedo and height cube maps are
     baked from Q4/Q5's coarse levels. It adds the atmosphere ray march, cloud shell, ocean BRDF, rings and
     night-side emission (cities, lava), and writes exact reverse-Z depth.
  3. **Rasterized terrain** (Q4's chunk LOD), once the body is large on screen, using the same baked maps for color
     continuity, with a screen-space dithered cross-fade over a band of projected sizes.

### 2.9 Depth precision from cockpit to orbit

| Option | Verdict |
|---|---|
| **Reverse-Z float32, infinite far (have it)** | Relative precision about 2^-23 of distance: about 0.01 m at 100 km and 12 m at 1e8 m. In Reed's test (near 0.1, far 10K) it gives 0% indistinguishable and 0% swapped, against 45% and 18% for standard depth ([Reed 2015](https://www.reedbeta.com/blog/depth-precision-visualized/)). Enough for one surface at any distance. |
| Logarithmic depth | Needs fragment depth for large triangles, which disables early-z: 8 to 10% fps lost on screen-covering objects ([Outerra](https://outerra.blogspot.com/2012/11/maximizing-depth-buffer-range-and.html)). On Apple TBDR it also defeats HSR. Reverse float32 is already "slightly better than 24-bit log". **No.** |
| Multi-frustum / depth partitioning | Cesium drew objects that straddle boundaries twice, 26 of 137 draws ([Cesium](https://cesium.com/blog/2018/05/24/logarithmic-depth/)). Not needed with reverse-Z float and analytic far bodies. **No.** |
| **Camera-relative rendering** (Q1) | The real fix. Unity HDRP does it by default ([docs](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/Camera-Relative-Rendering.html)); UE5 calls translated world space "safe to use floats" ([docs](https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5?lang=en-US)). Required, not optional. |

The risk is float32 positions, not depth: the scope's finding, confirmed. Every vertex, light, shadow cascade, cluster
and probe position the GPU sees is camera-relative float32. Far bodies are analytic, so their geometry never meets a
float32 vertex.

### 2.10 Exposure from deep space to caves

- **Histogram auto-exposure** in compute: 128 bins over about 30 EV, because starlight to sun is about 1e9
  ([Narkowicz](https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/), [Lux](https://en.wikipedia.org/wiki/Lux)).
  It uses percentile clipping (UE: low 70 to 80%, high 80 to 95%,
  [docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/auto-exposure-in-unreal-engine)) and separate
  adaptation speeds in stops per second.
- **The exposure stays on the GPU**: a one-element buffer the display pass reads, with no CPU readback, so nothing
  stalls and VCH-073 is not needed. MCP reads it a frame late through the frame-slot readback ADR 0030 already allows.
- **Pre-exposure** (Frostbite): shading multiplies radiance by last frame's exposure before writing `rgba16_float`,
  which fixes the sun-disk overflow.
- The physical camera of ADR 0037 stays. A manual `ev100` overrides, and an `AutoExposure` component sets the range,
  speeds and compensation.
- Stars are physically invisible at daylight exposure. The stylized look gets an explicit star and deep-sky boost in
  grading, not a fake exposure.

### 2.11 Scale s, feature by feature

**The law** (derived in the notes): optical depth τ = ∫σ ds. If every length of a medium is scaled by k and its
coefficients by 1/k, τ along any path that scales with it is unchanged, so the look is unchanged. A path that doesn't
scale (a 1 km line of sight to a house) gets 1/k the optical depth it should: 10x hazier at k = 0.1.

That splits the atmosphere into **three knobs**, not one:

- **s_p** scales the radius and sea level (Q1/Q3 own it).
- **s_atm** scales the atmosphere's top, scale heights and cloud-layer heights, with coefficients × 1/s_atm.
  - s_atm = s_p keeps the planet-from-orbit look exactly invariant. At NMS-like compression, though, the layer gets
    too thin to hold clouds and flight: at s_p = 0.01 Earth's atmosphere is 1 km thick, while HZD's cloud layer sits
    at 1.5 to 4 km.
  - KSP's Kerbin (radius 600 km, about 0.094 of Earth's, atmosphere 70 km, about 0.7 of Earth's 100 km; from memory,
    unverified) is the precedent for scaling the atmosphere much less than the radius.
  - With s_atm > s_p the vertical sky from the ground is unchanged (τ along the vertical is kept). The limb seen from
    orbit gets relatively thicker, which suits a stylized look.
- **Human-scale haze** fixes the 1/k problem for meter-scale distances. Aerial perspective toward nearby geometry
  integrates with **physical** coefficients (σ × s_atm, i.e. unscaled) out to a distance tied to the scale height,
  then blends to the scaled coefficients. Near the player, 1 km of air looks like 1 km of air; mountains 50 km away
  (which do scale) are hazed like the sky. This is a design proposal to validate in spike S3 (section 10).

| Feature | Follows s_p | Follows s_atm | Never scales (meters, physical) |
|---|---|---|---|
| Atmosphere | radius, horizon | top, scale heights, coefficients (× 1/s_atm) | near-field haze (via the human-scale band), sun illuminance (physical luminosity and distance, scope §2) |
| Clouds | weather map in body UV (storm sizes) | layer altitudes and thickness, extinction | noise detail frequency, raindrop and particle sizes |
| Ocean | sea-level radius | — | wave spectrum, wind, foam, underwater fog |
| Shadows | analytic planet, ring and eclipse occluders (geometry) | — | cascade split distances, filter sizes |
| Terrain and vegetation | large features (Q4) | — | detail, density per m², AO radius, probe spacing |
| Far-body LOD | nothing: tiers chosen by pixels | — | — |
| Starfield, deep sky | — | — | invariant in light-years (scope §1) |
| Exposure, AgX, grading | — | — | invariant |
| Depth | the worst case is s = 1 (gas giant at 1e8 m) | — | near plane |

Separate s_p and s_sys change angular sizes, and so eclipse geometry and the sun's apparent disk. The renderer takes
angular radius from the scaled geometry Q2 hands it and illuminance from physical values. It does not care which
factor produced them.

## 3. Tradeoffs

**Base renderer:**

| | A. Visibility buffer + clustered + forward transparents | B. Clustered forward | C. Deferred (G-buffer) |
|---|---|---|---|
| Complexity | Exists: two passes, tested on Metal and WARP. Adds culling, binning and the forward transparent path | A new path; rewrites the shade logic as fragment shaders; materials compile per pipeline | A new path; G-buffer layout becomes a contract |
| Performance, vegetation | Best: masked overdraw costs an alpha fetch, lighting runs once per pixel | Worst on TBDR: masked overdraw runs the full shader | Middle: masked overdraw writes the G-buffer |
| Performance, MSAA | Poor (per-sample shading) | Best (on-tile resolve on Apple) | Poor |
| Determinism | Same as today. GPU output is visual-only (Q12) | Same | Same |
| Extensibility | Materials as one compute switch (exists); new kinds compile into the shade kernel; binning keeps divergence bounded | Per-material pipelines are natural | Fixed G-buffer limits new material models |
| Testability | Existing goldens; one shade kernel to inspect with MCP | New goldens | New goldens |
| Scale range | Neutral: all three use camera-relative float32 | Neutral | Neutral |

**Temporal foundation (TAA, cloud reprojection, GTAO accumulation):**

- *Complexity:* motion vectors from every geometry feature (terrain, vegetation wind, skinned creatures), history
  targets, cut detection.
- *Performance:* clouds about 10x cheaper; GTAO and TAA cost under 1 ms.
- *Determinism:* headless runs step 1/60 s with a jitter sequence indexed by frame, so a recording replays to the
  same pixels.
- *Testability:* goldens need N warm-up frames and a fixed frame count, plus a `temporal = off` debug view to test
  single-frame features alone.
- *Extensibility:* every new feature must write motion vectors or be marked as not needing them.
- *Scale range:* none.

**Atmosphere, Hillaire generalized vs Bruneton:** Hillaire is cheaper to rebuild by three orders of magnitude, so
bodies can change parameters freely and N bodies cost nothing to keep. It is slightly less accurate in dense
atmospheres. Bruneton is spectral and more accurate, but 250 ms per rebuild and 8 MB per body. Both work at any scale,
because the scale law applies to coefficients, not the technique.

**Shadows, CSM vs VSM:** CSM is simple, fits today's RHI and is cheap on the M1, but wastes resolution and shimmers
without care. VSM gives better quality per byte, but needs VCH-071, VCH-075-class page logic and stable geometry.
Wind-animated foliage invalidates its cache, and our worlds are full of it.

**Far layer, analytic impostors vs rasterized distant terrain:** analytic is exact at any distance with no precision
risk, costs few pixels, and can be tested with a single screenshot. Its weakness is the handoff band, where two
representations must agree; a golden pair (just before and after) catches regressions.

## 4. Extension points

**Today's seam:** `Renderer::add_extractor(name, fn)` copies components into `RenderWorld`.
`Renderer::add_feature(RenderFeature)` has `prepare()` and `record(graph, frame)`. The graph orders passes by declared
access, with declaration order as the tie-breaker ([0029](../adr/0029-render-graph-sync-and-barriers.md)). It is
the right shape, with three gaps:

1. **A frame blackboard of well-known resources.** Features need more than `scene_color`: `hdr_color` (pre-exposed
   `rgba16_float`), `depth`, `visibility`, `motion`, `light_grid`, `shadow_cascades`, `sky` (`SkyFrame` and LUTs per
   body), `exposure`, `camera_medium`, `far_layer`. They are named handles in `FrameContext`, declared like any other
   access, so barriers and residency stay automatic.
2. **Stages for writes to shared targets.** Ordering by access works for producers and consumers. When two features
   blend into `hdr_color`, the order must be explicit. The stages are `frame_setup` (LUTs, culling, light grid),
   `shadows`, `opaque` (contribute geometry to the visibility buffer), `lighting_inputs`, `far_layer`, `transparent`,
   `volumetric`, `post_hdr`, `post_display`. A feature registers passes per stage; within a stage, registration order
   decides.
3. **Shading contributions as data, not code.** A module affects lighting by adding records to buffers the shade and
   forward passes already read: lights (planetshine is a directional light), analytic occluders (spheres, rings),
   decals, local fog volumes. No feature patches the shade kernel.

| What | Luau | C++ module | Toolbox core (`space/`) | Engine (generic, ADR 0042 "goes down") |
|---|---|---|---|---|
| New material look (terrain layers, foliage, creature skin) | no (pick by name in glTF/components) | yes: `veach_add_material` Slang kind ([0035](../adr/0035-material-model.md)) | — | the interface |
| Atmosphere, clouds, ocean, ring, gas-giant parameters per body | yes: components, set from procgen hooks (Q5) | yes | the mapping body record → render records | the features: spherical atmosphere around any center, cloud shell, spherical ocean, analytic sphere/ring impostor |
| A new phenomenon (aurora, ion storm, black-hole lensing, eruption ash) | data only, once a module provides it | yes: `RenderFeature` + extractor, at a stage | — | — |
| Extra lights, occluders, fog volumes, decals | yes: components | yes | eclipse occluders from Q2 | the buffers and their consumers |
| Starfield and deep-sky content | star "skins" and grading as data | a deep-sky feature (nebula volumes) | galaxy query → star list and background bake (Q3) | point-source list rendering, cubemap background |
| Far-body tiers and handoff policy | thresholds as settings | — | yes (needs Q4 terrain and body records) | the impostor primitive |
| Grading and stylization | yes: `ColorGrade` component (LUT, saturation, star boost) | custom post feature | — | the display pass |
| Exposure | yes: `AutoExposure` and `Camera` components | — | — | the histogram feature |

Luau never supplies shader code. Runtime shader compilation (VCH-009) could later let creators write material
snippets, a (b) item.

## 5. Interactions with the other questions

- **Q1 coordinates:** Q8 requires camera-relative float32 for everything the GPU sees. Extraction subtracts the camera
  in double or int64 and hands float offsets (vertices relative to a chunk origin, plus a per-chunk float offset).
  Far bodies arrive as direction plus double distance, and the impostor intersects in a rescaled frame.
  `RenderView` gains a world-space camera position in Q1's type. Photo mode's TLAS takes the same camera-relative
  transforms. **Conflict to settle:** Q1 must not choose a design where render positions are float32 world space
  anywhere.
- **Q2 mechanics and time:** sun directions, angular radii and visible fractions (eclipses), the body's rotation (to
  orient impostor maps and the atmosphere's local frame), and N suns. Time of day is no longer a sky input: it falls
  out of geometry. The `Sky` component's `time_of_day`, `latitude` and `day_of_year` become a flat-world fallback.
- **Q3 generation:** the `AtmosphereRecord` from composition (also used by Q10's drag and audio: **one record**), star
  luminosity and temperature, the ring radial profile, gas-giant band parameters, and the galaxy query for the
  starfield and background bake.
- **Q4 terrain:** chunks meshed on the GPU and drawn with indirect draws (VCH-071), meshlet-sized clusters into the
  visibility buffer, a chunk-ready hook for vegetation scattering, texture arrays for material layers (VCH-072),
  coarse albedo and height for impostor bakes, and the handoff band. Terrain motion vectors exist only as camera
  motion (terrain is static).
- **Q5 procgen graph:** weather maps, band textures, vegetation placement (GPU, visual-only) and impostor bakes are
  graph outputs. Q8 only consumes textures and buffers.
- **Q6 networking:** nothing rendered is synced. Weather parameters and time are; the server never needs a GPU.
- **Q7 simulation LOD:** weather state drives cloud coverage maps. Eruptions emit particles, ash volumes and emissive
  lava as data.
- **Q9 physics:** none directly. The camera medium needs "is the camera underwater or inside the atmosphere", which is
  a query on Q3/Q4 data, not a physics query.
- **Q10 flight and travel:** the approach speed sets how fast the impostor → terrain handoff must stream (Q4's
  budget). Reentry heating VFX reads the shared atmosphere record. Speed tiers decide how often the starfield
  background rebakes.
- **Q11 persistence:** render data is never saved. Baked impostor and background textures are caches keyed by the
  generator version and seed, so a generator change invalidates them automatically.
- **Q12 determinism:** GPU-generated vegetation, clouds and bakes are visual-only and must never feed gameplay. TAA
  jitter and exposure adaptation are frame-indexed, so record/replay reproduces images on the same machine.
  Cross-vendor pixel equality is not promised: goldens compare with FLIP tolerance, as today.

## 6. M1-class vs M3 Pro-class: budget

**Scaling method (estimates).**

- M1: 8 cores, 2.6 TFLOPS, 68 GB/s. M3 Pro: 18 cores, about 6.4 TFLOPS (third party), 150 GB/s. Our M5 Max: 32
  cores, 460 GB/s.
- From those: **M1 time ≈ 6 × M5 Max time (range 5 to 8). M3 Pro time ≈ 2.5 × M5 Max time (range 2 to 3).**
- PS4-era budgets (1.84 TFLOPS, 176 GB/s) are taken as roughly M1-class.
- None of this has been measured on an M1 or an M3 Pro.

**Per-feature GPU budget at 60 fps (16.7 ms).** Every number is an estimate unless marked measured.

| Feature (worst-case scene: ocean, clouds, vegetation, one shadowed sun) | M5 Max 1080p | M1 1080p | M3 Pro 1080p | M3 Pro 1440p | Basis |
|---|---|---|---|---|---|
| GPU culling + visibility (terrain, props, vegetation) | 0.5 | 3.0 | 1.3 | 1.6 | Wihlidal; measured uncull'd Bistro 5.7 ms shows what culling must remove |
| Cascaded shadows, 3 × 2048², one sun | 0.4 | 2.5 | 1.0 | 1.1 | geometry-bound; estimate |
| Shade: materials, clustered lights, CSM, SH ambient, AO, aerial perspective | 0.8 measured (today's simpler shade) | 4.5 | 2.0 | 3.5 | measured × factors, plus lighting |
| GTAO, half res, temporal | 0.1 | 0.7 | 0.3 | 0.5 | Jimenez: 0.5 ms PS4 1080p |
| Atmosphere: LUTs, sky, aerial perspective (one body, N suns) | 0.17 to 0.25 measured | 1.0 | 0.4 | 0.6 | measured; Hillaire 0.31 ms GTX 1080 720p |
| + atmosphere ray march from space (planet on screen) | 0.1 | 0.5 | 0.2 | 0.35 | Hillaire 0.5 ms total from space |
| Volumetric clouds, quarter-res trace + reprojection | 0.5 | 3.0 | 1.2 | 1.8 | HZD 2 ms PS4; Nubis ≤ 4 ms PS4 |
| Ocean: FFT bands, surface, underwater fog | 0.25 | 1.5 | 0.6 | 0.9 | Atlas 0.5 ms RTX 2080 |
| Forward transparents (glass, particles) | 0.15 | 1.0 | 0.4 | 0.7 | estimate |
| Far layer: stars, background, impostors, rings | 0.05 | 0.3 | 0.15 | 0.2 | few pixels; estimate |
| TAA | 0.08 | 0.5 | 0.2 | 0.35 | estimate |
| Auto-exposure histogram | 0.02 | 0.1 | 0.05 | 0.05 | estimate |
| Bloom, grading, AgX display | 0.05 to 0.19 measured (display only) | 0.8 | 0.35 | 0.6 | measured + bloom estimate |
| **Total (planet surface, without the from-space row)** | **~3.2** | **~18.9** | **~7.95** | **~11.9** | |
| Left for Q4 meshing and Q5 GPU procgen on the same queue | | none | ~8 | ~4.5 | |

What this says:

- **An M3 Pro-class GPU runs the full feature set at 1440p60** with room for terrain meshing and procgen compute (the
  estimate leaves about 4.5 ms).
- **A base M1 does not run the full set at 1080p60.** About 19 ms before any terrain compute means 30 fps. A low
  tier makes 1080p60 plausible:
  - 2D cloud layers instead of volumetric (−2.8 ms);
  - 2 × 1024² cascades (−1.2);
  - shading at 0.75 render scale with a spatial upsample (−1.5; this is not a temporal upscaler);
  - three ocean bands (−0.4).
  That totals about 13 ms. It means bringing back quality tiers, which the reset deleted ([0001](../adr/0001-reset.md)).
- **Async compute (VCH-074)** matters more on the M1, where meshing has no other room. It is not a prerequisite for
  the M3 Pro target.

**What can be verified on the M5 Max:**

- Correctness, everything: goldens, MCP screenshots, the scale invariance of the atmosphere (S3), depth at 1e8 m.
- Relative costs: which pass dominates, and how a feature scales with resolution and with N suns or N bodies.
- Absolute numbers for an M1 or an M3 Pro **only** by the factors above. Running the M5 Max at 4K approximates the
  per-pixel load but not the bandwidth or occupancy of a smaller GPU.

The cheapest way to replace the estimates is one measurement session on a real base M1 (product decision 1).

## 7. Recommendation, and what would change my mind

1. **Keep the visibility buffer as the base, and grow it.**
   - Opaque and masked geometry into the visibility buffer, drawn opaque first, then masked.
   - GPU meshlet culling (frustum, cone, Hi-Z) with indirect draws.
   - Compute shading binned by material kind.
   - A clustered light grid (z-binned, Drobot-style) shared by the shade pass and a **forward pass for transparents**
     (water surface, glass, particles), which needs blending in the RHI.
   - Supersede ADR 0040. Option B loses on our dominant content (masked vegetation on TBDR), and option C on
     portability.
2. **Bring back a native-resolution temporal foundation:** deterministic jitter, motion vectors, history, TAA, no
   upscaling. Clouds, GTAO and foliage AA need it. It reverses part of the reset, so the owner decides
   (section 11).
3. **Shadows: stable CSM for at most one or two suns, analytic occluders for everything planet-sized,** a cloud
   shadow map. No virtual shadow maps.
4. **Indirect light:** an SH sky ambient per frame, GTAO, a prefiltered sky cubemap, local probes, SSR as an M3 tier
   option.
5. **Atmospheres: generalize Hillaire.**
   - Any center and radius, local up from the body's center.
   - The LUT path inside the atmosphere, a per-pixel ray march outside it and for other bodies.
   - All suns summed in one absolute-azimuth sky-view table.
   - Coefficients from Q3's composition tables.
   - **s_atm separate from s_p**, plus the **human-scale haze** band.
   - Supersede ADR 0038; the `Sky` component stays as the flat-world mode.
6. **Clouds:** a Nubis-style shell ray march with reprojection on the M3 tier, 2D layers on the M1 tier and from
   orbit.
7. **Oceans:** Atlas-style FFT bands on Crest-style rings over a sea-level sphere, with the Bruneton filtered BRDF from
   altitude. Underwater, vacuum and deep atmosphere are a **camera medium**.
8. **The far layer:** energy-conserving stars from Q3's galaxy query, a deep-sky background baked per system, and
   three-tier bodies (point, analytic impostor, terrain) chosen by projected pixels. Rings and gas giants are
   analytic.
9. **Depth:** keep reverse-Z infinite float32. No log depth, no multi-frustum. Camera-relative everything.
10. **Exposure:** histogram auto-exposure that stays on the GPU, pre-exposed HDR, the physical camera as manual
    override, a star boost in grading.
11. **RenderFeature:** add the frame blackboard, stages, and shading contributions as data (section 4).

**What would change my mind:**

- **B instead of A:** if the culled visibility buffer plus binned shading measures slower than a clustered forward
  prototype on a vegetation field on the M5 Max (spike S1), or if MSAA proves necessary because TAA ghosting on thin
  foliage is unacceptable by eye. A product "no temporal" answer also weakens A: without TAA, B with MSAA and
  alpha-to-coverage is the only real foliage AA.
- **Bruneton:** if Hillaire's multiple-scattering approximation looks wrong for the dense or hazy atmospheres the
  creators want, compared against photo mode's path-traced sky.
- **The temporal foundation:** if reprojected clouds and TAA show artifacts on fast ship flight that grading cannot
  hide. Then clouds go back to 2D layers plus cheap volumetric fog on every tier.
- **The hardware target:** if a real M1 measures much better than the factor of 6.

## 8. What is expensive to undo

- **The base renderer.** Materials compile into a compute shade kernel (A) versus fragment pipelines (B). Every
  material, debug view and golden follows. Decide once, with S1.
- **The frame blackboard and stage API.** Every feature module is written against it.
- **The temporal foundation.** Once clouds, AO and AA depend on history and every geometry feature writes motion
  vectors, removing it means re-budgeting each of them.
- **The `AtmosphereRecord` units and the s_atm and haze model.** The record is shared with Q10's drag and Q3's
  generation, and creators tune looks against it. Changing its meaning re-tunes every world.
- **Camera-relative positions** across extraction, shadows and probes (with Q1).
- **The impostor-bake contract with Q4/Q5** (which maps, at what resolution, keyed by what).
- **Cheap to change later:** the AO algorithm, SSR, the cloud technique within the shell model, ocean bands, bloom,
  CSM details, the starfield's PSF.

## 9. Proposed ADR titles

1. The real-time renderer: a culled visibility buffer for opaque and masked geometry, clustered lights, forward
   transparents (supersedes 0040)
2. GPU-driven geometry: meshlet culling, indirect draws and material binning
3. A native-resolution temporal foundation: jitter, motion vectors, history and TAA, no upscaling (owner decision first)
4. Shadows: cascaded shadow maps for near geometry, analytic occluders for planets, rings and eclipses
5. Indirect light without GI: SH sky ambient, GTAO, sky and local reflection probes
6. Atmospheres per body: Hillaire LUTs around any center, ray-marched from space, N suns (supersedes 0038)
7. Scale in rendering: the coefficient law, s_atm, and human-scale haze
8. The far layer: starfield, deep-sky background, analytic body impostors and the handoff to terrain
9. Depth and precision: reverse-Z infinite float32 with camera-relative rendering; no log depth, no multi-frustum
10. Auto-exposure with pre-exposure (amends 0037)
11. RenderFeature stages, the frame blackboard, and shading contributions as data
12. The camera medium: air, water, vacuum and deep atmosphere as frame state
13. Quality tiers (owner decision first; follows the hardware target)

## 10. Spike candidates

Each spike is a throwaway prototype that validates one decision. They are ordered toward the first slice (a planet
seen from orbit with a composition-driven atmosphere, flying down, landing).

| Id | Validates | Smallest prototype (scope) | Visible success criterion |
|---|---|---|---|
| **S1** | A vs B for vegetation | A lab scene of about 200k instanced, alpha-tested cards over a plane. The existing visibility path, plus a throwaway forward fragment shader with the same material and lights. Requires a minimal indirect draw or CPU-side instancing. | MCP `gpu_timings` at 1080p and 1440p on the M5 Max: visibility + shade vs forward. FLIP of the two images < 0.05. Decision: A if its total is lower. |
| **S2** | Culling pays off before anything else | Frustum + cone culling of Bistro meshlets in compute, CPU-read counts first, indirect draw if VCH-071 is ready | Bistro's `visibility` pass at 1440p drops by at least half; the golden is unchanged by FLIP |
| **S3** | The atmosphere around any center, from space, at any s | Hillaire LUTs parameterized by body center and radius, the space ray march, a Luau script flying the camera from 2 m to 3 radii | Screenshots at the same relative altitude at s = 1 and s = 0.01 have FLIP < 0.05. The limb from orbit shows no seam at the LUT-to-ray-march switch. A 1 km-distant tower looks equally hazy at both s with the haze band on, and visibly hazier at s = 0.01 with it off |
| **S4** | N suns and eclipses | Two directional suns in one summed sky-view table; an analytic moon occluder from a hard-coded orbit | Screenshot of a double sunset with two shadows (one shadowed sun), sky cost per extra sun < 0.05 ms on the M5 Max, the eclipse's visible fraction matching the disk-overlap formula in a unit test |
| **S5** | Depth and far layer from cockpit to gas giant | A 0.5 m cockpit mesh, a camera-relative terrain patch, an analytic 70,000 km impostor at 1e8 m, a ring | Screenshot with no z-fighting or jitter while the camera moves 1 cm per frame at 1e8 m from the origin (camera-relative on) and visible jitter with it off |
| **S6** | Clouds need the temporal foundation | A shell ray march with 3D noise (needs VCH-072, or a 2D-atlas stand-in) at quarter res, with and without 1/16 reprojection | `gpu_timings` showing the reprojected cost at or below 1/5 of the full cost; screenshots after 16 frames without visible noise |
| **S7** | Auto-exposure across the range, pre-exposure | Histogram exposure on the lab's Bistro, plus a cave (closed box) and a starfield-only view | A sequence of screenshots (sunlit, cave, deep space, looking at the sun) all well exposed. No non-finite pixels with the sun disk on screen (`render.nonfinite_samples`-style check) |
| **S8** | The budget factors | The same fixed benchmark scenes (S1, S3, S6) run on a real base M1 and an M3 Pro-class machine | One table of measured ms replacing section 6's estimates |

S1, S2, S3 and S5 need no new RHI work, apart from S1's optional indirect draw. S6 needs 3D textures (VCH-072) or an
atlas. S8 needs hardware we don't have.

**RHI gaps by option** (existing items plus new ones this research found, ids to be assigned by `/backlog`):

| Gap | Needed by |
|---|---|
| VCH-071 indirect draw/dispatch, indexed draws | culling (S2), material binning, vegetation, GPU terrain (Q4) |
| VCH-072 3D textures, arrays, cube maps, more formats | clouds, aerial perspective as a real volume, CSM arrays, sky cubemap and probes, terrain layers, r16f/rg16f for motion and AO |
| VCH-074 async compute | the M1 tier's terrain meshing; not blocking for the M3 Pro target |
| VCH-076 texture updates | impostor and background bakes, gas-giant bands, streamed terrain textures |
| VCH-073 non-blocking readback | not needed by the renderer (exposure stays on the GPU) |
| VCH-075 wider atomics | not needed, unless a software rasterizer is ever wanted (and then not on the M1) |
| **new:** blend state in graphics pipelines | forward transparents: water, glass, particles, far-layer compositing |
| **new:** depth bias, comparison samplers, anisotropic filtering | shadows, terrain and foliage texture quality |
| **new (only if option B):** MSAA render targets and resolve | B's AA |

## 11. Product decisions surfaced

1. **The hardware target.** *Recommendation:* M3 Pro-class at 1440p60 with the full feature set as the design target,
   and base M1 at 1080p60 on a low tier (or 1080p30 at full quality) as the minimum. Spend once on a used base M1
   machine to replace the estimates (S8). ADR 0003's "60 fps 1080p on a base M1" with every feature does not fit by
   these estimates (about 19 ms).
2. **Quality tiers.** They come back with the target above (deleted at the reset). *Recommendation:* yes, two tiers to
   start (M1, M3 Pro), with the features and settings in section 6.
3. **A native-resolution temporal foundation (TAA, reprojection), still no upscalers.** *Recommendation:* yes. It is
   the only way volumetric clouds fit any budget, and the realistic AA for dense foliage. It reverses part of the
   reset's removal of temporal history.
4. **A spatial render scale for the M1 tier** (shading below output resolution with a non-temporal upsample).
   *Recommendation:* yes, as a setting. It is not a temporal-upscaler dependency.
5. **The atmosphere's scale knob s_atm and the human-scale haze.** *Recommendation:*
   - s_atm is a creator setting, defaulting to s_p for s_p ≥ 0.1 and to a floor of about 10 km of atmosphere top below
     that, so clouds and flight have room;
   - human-scale haze is on by default.
   This changes how planets look from orbit at strong compression (thicker limbs), which matches the NMS reference.
6. **Stylization.** *Recommendation:* PBR plus a grading step (LUT, saturation, a star and deep-sky boost) exposed as
   Luau-settable components, with no non-physical lighting model (consistent with ADR 0039).

## 12. Sources

Full annotations and numbers: [notes/08-renderer-notes.md](notes/08-renderer-notes.md). Local measurements: M5 Max,
MCP `gpu_timings`, interim path (notes, first section).

Base renderer, culling, materials:

- Burns & Hunt 2013, The Visibility Buffer (JCGT): https://jcgt.org/published/0002/02/04/paper.pdf
- Schied & Dachsbacher 2015, Deferred Attribute Interpolation: https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf
- Wihlidal, GDC 2016, Optimizing the Graphics Pipeline with GPU Compute: https://gdcvault.com/play/1023109/Optimizing-the-Graphics-Pipeline-With,
  https://www.slideshare.net/slideshow/optimizing-the-graphics-pipeline-with-compute-gdc-2016/59747720
- Engel, GDC 2016, Filtered and Culled Visibility Buffer: https://www.slideshare.net/slideshow/triangle-visibility-buffer/238860894
- The Forge v1.57: https://github.com/ConfettiFX/The-Forge/releases/tag/v1.57
- Karis et al., SIGGRAPH 2021, Nanite: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf;
  UE Nanite docs: https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine;
  Nanite materials notes (Wihlidal GDC 2024): https://scthe-github-io.vercel.app/blog/nanite-materials-notes;
  Arm on Nanite's visibility buffer: https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-and-unreal-engine-s-nanite-enabling-the-future-of-mobile-graphics;
  64-bit atomics on Apple GPUs (forum, unverified): https://forums.unrealengine.com/t/lumen-nanite-on-macos/508411/53
- Geffroy, Gneiting & Wang, SIGGRAPH 2020, Rendering the Hellscape of Doom Eternal: https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf;
  Doom 2016 study: https://adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study
- Olsson, Billeter & Assarsson, HPG 2012, Clustered Deferred and Forward Shading: https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf
- Drobot, SIGGRAPH 2017, Improved Culling for Tiled and Clustered Rendering (CoD IW): https://www.advances.realtimerendering.com/s2017/2017_Sig_Improved_Culling_final.pdf
- Haar & Aaltonen, SIGGRAPH 2015, GPU-Driven Rendering Pipelines (AC Unity): https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
- van Muijden, GDC 2017, GPU-Based Procedural Placement in Horizon Zero Dawn: https://www.guerrilla-games.com/media/News/Files/GDC2017_VanMuijden_GPUBasedProceduralPlacementInHorizonZeroDawn.pdf
- Wohllaib, GDC 2021, Procedural Grass in Ghost of Tsushima: https://www.gdcvault.com/play/1027214/Advanced-Graphics-Summit-Procedural-Grass

Apple GPUs:

- Apple, Tailor your apps for Apple GPUs and TBDR: https://developer.apple.com/documentation/metal/tailor-your-apps-for-apple-gpus-and-tile-based-deferred-rendering
- WWDC20 Harness Apple GPUs with Metal: https://developer.apple.com/videos/play/wwdc2020/10632
- M1: https://www.apple.com/newsroom/2020/11/apple-unleashes-m1/; M3 family: https://www.apple.com/newsroom/2023/10/apple-unveils-m3-m3-pro-and-m3-max-the-most-advanced-chips-for-a-personal-computer/;
  M3 Pro bandwidth: https://www.macrumors.com/2023/10/31/apple-m3-pro-less-memory-bandwidth/;
  M3 Pro GPU: https://www.notebookcheck.net/Apple-M3-Pro-18-Core-GPU-Benchmarks-and-Specs.782445.0.html;
  M5 Pro/Max: https://www.apple.com/newsroom/2026/03/apple-debuts-m5-pro-and-m5-max-to-supercharge-the-most-demanding-pro-workflows/;
  MacBook Pro specs: https://www.apple.com/macbook-pro/specs/

Anti-aliasing, AO, exposure:

- No Man's Sky on Mac: https://www.nomanssky.com/2023/06/no-mans-sky-on-mac; player reports: https://steamcommunity.com/app/275850/discussions/0/4526764179299716050
- Karis 2014, High Quality Temporal Supersampling (unverified): https://advances.realtimerendering.com/s2014/
- Jimenez et al. 2016, GTAO: https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf
- Lagarde & de Rousiers 2014, Moving Frostbite to PBR: https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v32.pdf
- UE Auto Exposure: https://dev.epicgames.com/documentation/en-us/unreal-engine/auto-exposure-in-unreal-engine
- Narkowicz 2016, Automatic exposure: https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/
- Lux levels: https://en.wikipedia.org/wiki/Lux; apparent magnitude: https://en.wikipedia.org/wiki/Apparent_magnitude
- UE Virtual Shadow Maps (unverified, page empty when fetched): https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine

Atmosphere, clouds, ocean:

- Hillaire 2020, A Scalable and Production Ready Sky and Atmosphere Rendering Technique: https://sebh.github.io/publications/egsr2020.pdf;
  sample code: https://github.com/sebh/UnrealEngineSkyAtmosphere
- UE Sky Atmosphere: https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine
- Bruneton & Neyret 2008, Precomputed Atmospheric Scattering: https://inria.hal.science/inria-00288758/document;
  Bruneton 2017: https://ebruneton.github.io/precomputed_atmospheric_scattering/
- Hillaire 2016, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite: https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
- SpaceEngine atmosphere news (2010): https://spaceengine.org/news/blog100301/
- Schneider, SIGGRAPH 2015, Horizon Zero Dawn clouds: https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
- Nubis 2017: https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf;
  Nubis Evolved 2022: https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf;
  Nubis Cubed 2023: https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf
- UE Volumetric Cloud: https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine
- Toft et al. 2016, clouds without temporal reuse: https://arxiv.org/pdf/1609.05344
- No Man's Sky Worlds Part I: https://nomanssky.com/worlds-part-i-update
- Tessendorf, Simulating Ocean Water: https://people.computing.clemson.edu/~jtessen/reports/papers_files/coursenotes2004.pdf
- Atlas, GDC 2019: https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd6-interactive-water-simulation-in-atlas.pdf
- Crest: https://crest.readthedocs.io/en/4.23.0/user/system-notes.html, https://crest.readthedocs.io/en/4.23.0/user/waves.html
- Bruneton, Neyret & Holzschuch 2010, ocean lighting from geometry to BRDF: https://evasion.inrialpes.fr/Publications/2010/BNH10

Space-scale precedents and depth:

- Reed 2015, Depth Precision Visualized: https://www.reedbeta.com/blog/depth-precision-visualized/
- Outerra, logarithmic depth: https://outerra.blogspot.com/2012/11/maximizing-depth-buffer-range-and.html
- Cesium, logarithmic depth and multi-frustum: https://cesium.com/blog/2018/05/24/logarithmic-depth/
- Unity HDRP camera-relative rendering: https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/Camera-Relative-Rendering.html
- UE5 Large World Coordinates rendering: https://dev.epicgames.com/documentation/unreal-engine/large-world-coordinates-rendering-in-unreal-engine-5?lang=en-US
- KSP ScaledSpace API: https://anatid.github.io/XML-Documentation-for-the-KSP-API/class_scaled_space.html
- Elite Dangerous Stellar Forge: https://www.space.com/31366-elite-dangerous-stellar-forge-interview.html; skybox seams:
  https://forums.frontier.co.uk/threads/borders-between-codex-regions-seem-to-also-be-borders-between-different-background-star-fields.616573
- Star Citizen Planet Tech v5 (press level): https://starcitizen.tools/Planet_Tech_V5
- Gas giants: https://smcameron.github.io/space-nerds-in-space/gaseous-giganticus-slides/slideshow.html, https://jasper-r.github.io/gas-giant

Not found or not verified in this session (see the notes): NMS rendering talks, Star Citizen and Elite Odyssey
atmosphere internals, Outer Wilds' atmosphere, SpaceEngine's starfield and depth approach, ring-shadow and eclipse
practice in shipped games, Sea of Thieves water, foliage AA comparisons, Kerbin's figures.
