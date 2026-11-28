# SP-10: a per-body atmosphere from the ground to orbit (VCH-086)

Validates the atmosphere part of [ADR 0063](../../adr/0063-astronomical-rendering.md) (Hillaire generalized to any
body, a per-pixel march from space with no seam, N suns, eclipses) and the atmosphere scale rule of
[ADR 0049](../../adr/0049-scale-model.md) (s_a = √s_p, coefficients × 1/s_a). Starting point:
[ADR 0038](../../adr/0038-physical-sky.md)'s sky. Research: [08-renderer](../08-renderer.md) §2.5, §2.11, spikes S3 and S4.

**Verdict.**

- **ADR 0063's technique holds.** One integrator serves the sky-view LUT inside the atmosphere and a per-pixel
  march outside it, around a planet whose center and up come from the camera-relative frame. The switch at the top
  is invisible: mean FLIP 0.004 to 0.009 between the two sides, for both compositions at both scales.
- **Two suns cost 0.019 ms each on the M5 Max**, under the 0.05 ms target. An eclipse works with an analytic
  occluder evaluated per sample, which also casts the moon's shadow on the planet seen from orbit.
- **The march from space costs more than the ADR's estimate.** It adds 0.2 to 0.6 ms at 1080p (32 steps; 0.1 to
  0.35 ms at 16 steps), against the ADR's 0.1 ms.
- **The s_a = √s_p rule keeps the high-sun sky but not sunsets or limbs.** Horizontal optical depth scales as
  s_p^¼, so at s = 0.01 a sunset is far paler: blue transmittance with the sun 1.5° up rises from 0.006 to 0.103.
  This trips 0049's "reopen if VCH-086's √s atmosphere looks wrong from orbit or ground" clause. It is a product
  call, packet below.

## What was built

| File | What |
|---|---|
| `engine/render/shaders/veach/atmosphere.slang` | `AtmosphereParams` with per-channel Mie scattering and absorption; `SkyFrame` gains a planet block; `planet_march` (the one integrator), the absolute-azimuth sky-view mapping, `planet_sun_visible` (disk overlap), `planet_sky_radiance`, `sky_radiance_any` |
| `engine/render/shaders/atmosphere.slang` | the sky-view, aerial-perspective and irradiance LUT entries branch to the planet path when `planet = 1` |
| `engine/render/include/veach/render/atmosphere.hpp`, `src/atmosphere.cpp` | `PlanetSun`, `PlanetSky` (the seam: a module fills `ResolvedSky::planet` during extraction); `AtmosphereRenderer` writes the planet block, all suns scaled to the brightest |
| `engine/render/include/veach/render/renderer.hpp` | `ResolvedSky::planet` (optional) |
| `engine/render/shaders/realtime_shade.slang` | the sky pixels call `sky_radiance_any` |
| `space/include/veach/space/atmosphere.hpp`, `src/atmosphere.cpp` | composition → `AtmosphereRecord` (Rayleigh from refractivity and King factor, an aerosol table entry, an absorber column), `scaled(record, s_p)` (the s_a rule), `vertical_optical_depth`, Earth/Mars/Titan-like compositions, `disk_visible_fraction` |
| `tests/sp10_atmosphere_tests.cpp` | `[sp10.rayleigh]`, `[sp10.composition]`, `[sp10.scale]`, `[sp10.eclipse]` |
| `apps/lab/src/sp10_spike.cpp` | the lab module: `VEACH_SP10_*` configuration, the camera at the origin over a planet whose up is tilted off world +Y, the suns (one, two from `space::reference_system()`, two or four hard-coded), the moon, and the extractor |
| `docs/research/spikes/sp-10/scripts/` | `shots.py` (every screenshot below), `compare.py` (the FLIP tables), `timings.py` (GPU ms over MCP), `montage.py` |

**The seam to the engine.** With `ResolvedSky::planet` empty nothing changes: the Sky path is the same code, and
ten of the eleven golden outputs are byte-identical to the baseline. The eleventh, the photo-mode Cornell box, has
`Sky.mode = none` and differs between two runs of the same build too. The real-time renderer uses the planet path;
photo mode's escaped rays still call the Sky's `sky_radiance` (see follow-ups).

**How it works.**

- Positions are relative to the planet's center, in km, float.
  - Local up is `normalize(camera − center)`.
  - The camera's altitude is passed separately (from double on the CPU), so `|o|² − R²` never cancels at
    planetary radii. Ray-sphere tests use the stable quadratic.
- One function, `planet_march`, integrates in-scattering and transmittance along a ray clipped to the shell.
  - It sums every sun (per sample: transmittance LUT, phase, multiple-scattering LUT, visibility past the
    occluder) and adds the lit analytic ground where the ray meets it.
  - Inside the atmosphere it fills the sky-view LUT (192 × 108, 32 steps). The LUT is parameterized by absolute
    azimuth around local up from a reference axis (the first sun's horizontal direction) and by elevation squeezed
    toward the horizon.
  - Outside the atmosphere the shade pass calls it per pixel with the same 32 steps. `VEACH_SP10_MARCH` forces
    either path for the seam checks.
- The aerial-perspective atlas and the sky irradiance use the same march and frame. Only the readers' frame
  changed.
- **Eclipse.** `planet_sun_visible` is the flat-disk overlap of the sun and the occluder seen from the sample
  point. It mirrors `space::disk_visible_fraction`, which `[sp10.eclipse]` checks against a numeric integral within
  0.002. Evaluating it per sample gives a spatial umbra and penumbra, not a global dimming. The CPU uses the camera's
  fraction for the directional light that lights meshes.
- **A precision bug from orbit, fixed.** The first orbit images were speckled. A sample point near the ground,
  seen from 12,700 km, carries meters of rounding; one rounded under the ground "hit" the planet in `ray_sphere` and
  shadowed itself. The shadow test is now "the ray heads inward and passes closer than R", and the ground point is
  `n · R`. The real implementation needs the same care wherever it works in km floats at s = 1.

## Compositions

`atmosphere_from` takes mole fractions, surface pressure, temperature, gravity and radius, plus one aerosol and one
absorber. It computes:

- Rayleigh as σ = 24π³/(λ⁴N_s²)·((n²−1)/(n²+2))²·F_K per gas, at 680, 550 and 440 nm.
- The scale height as kT/(m̄g).
- The top at max(12 H_Rayleigh, 6 H_aerosol).

| | Earth-like (N2/O2/Ar, 1 atm, 288 K) | Mars-like (95% CO2, 610 Pa, 210 K, g 3.72) | Titan-like (N2/CH4, 1.47 bar, 94 K, g 1.35) |
|---|---|---|---|
| Rayleigh scale height | 8.43 km | 10.7 km | 20.8 km |
| Vertical optical depth (680, 550, 440 nm) | 0.056, 0.130, 0.242 (Rayleigh 0.0965 at 550, Bodhaine 1999: 0.0973) | 0.501, 0.503, 0.506 (dust 0.5, Rayleigh 0.0025) | 2.87, 4.09, 6.58 |
| Aerosol | Hillaire's (τ 0.0053, g 0.8) | dust τ 0.5, H 11 km, albedo 0.97/0.86/0.62, g 0.65 | haze τ 3, H 60 km, albedo 0.96/0.88/0.62, g 0.6 |
| Absorber | ozone 300 DU in Hillaire's tent | none | none (methane bands not modeled) |
| Thickness (physical) | 101 km | 130 km | 360 km |

- Hillaire's Rayleigh coefficients (13.558e-6/m at 550 nm) are 18.6% above air at 288 K: his optical depth is 0.108
  against the measured 0.0973.
- The composition Earth is therefore a slightly paler blue than ADR 0038's: mean FLIP 0.079, the same view
  ([Hillaire](sp-10/compare/hillaire_s1_ground.png), [composition](sp-10/compare/earth_s1_ground.png)). The ozone
  cross-sections were derived to give Hillaire's peak from 300 DU, not taken from a table.
- **Mars's dust albedos were chosen by eye** for a butterscotch sky. The sky reads tan with a whitish aureole.
  The real Mars blue sunset aureole needs a per-channel Mie phase (one g here), so it does not appear.

## Screenshots: ground → 10 km → 100 km → orbit

960 × 540, 60° vertical field of view, release build, Metal on the M5 Max.

- **View.** Below the top, the camera looks along local north with the horizon 40% of the half-height below
  center. In orbit (2 radii up, the whole planet in view) it looks at the center.
- **Sun.** One sun 25° up, 60° to the right, at the body's distance: 128,000 lux at Earth, 55,100 lux at Mars.
- **Exposure.** ev100 is fixed per composition (15 Earth, 14 Mars), so brightness changes along a sequence are
  real.

Strips (the same images, quarter size): [Earth](sp-10/strips/earth.png), [Mars](sp-10/strips/mars.png).

| | ground (2 m) | 10 km | 100 km | orbit |
|---|---|---|---|---|
| Earth, s = 1 (R 6371 km, top 101 km) | ![](sp-10/earth_s1_ground.png) | ![](sp-10/earth_s1_10km.png) | ![](sp-10/earth_s1_100km.png) | ![](sp-10/earth_s1_orbit.png) |
| Earth, s = 0.01 (R 63.7 km, top 10.1 km) | ![](sp-10/earth_s0.01_ground.png) | ![](sp-10/earth_s0.01_10km.png) | ![](sp-10/earth_s0.01_100km.png) | ![](sp-10/earth_s0.01_orbit.png) |
| Mars, s = 1 (R 3390 km, top 130 km) | ![](sp-10/mars_s1_ground.png) | ![](sp-10/mars_s1_10km.png) | ![](sp-10/mars_s1_100km.png) | ![](sp-10/mars_s1_orbit.png) |
| Mars, s = 0.01 (R 33.9 km, top 12.9 km) | ![](sp-10/mars_s0.01_ground.png) | ![](sp-10/mars_s0.01_10km.png) | ![](sp-10/mars_s0.01_100km.png) | ![](sp-10/mars_s0.01_orbit.png) |

Each row also has `_1km`, `_top-` (0.5% under the top, sky-view LUT) and `_top+` (0.5% over it, march) in
`sp-10/`.

What the images show:

- **Earth, s = 1.**
  - Ground: a blue sky brightening toward the sun's side and a pale horizon over brown-grey ground.
  - 10 km: a darker zenith and a hazy white band at the horizon.
  - 100 km: black space over a thin blue limb.
  - Orbit: a grey-white gibbous planet with a thin blue rim and a sharp terminator. It is grey, not Earth-blue,
    because the ground is a uniform 0.3 albedo: there is no ocean.
- **Earth, s = 0.01.**
  - Ground: almost the same sky as s = 1 (vertical optical depth is identical).
  - 10 km: the top of the atmosphere, already a curved limb. 100 km: a small planet.
  - Orbit: the same gibbous planet with a visibly thicker blue rim (10 km of air on a 64 km planet).
- **Mars.**
  - A tan, dusty, low-contrast sky with a bright whitish aureole around the sun. The ground is dark brown: the
    analytic ground gets direct sun only (31% through τ 0.5 at 25°), no skylight.
  - From orbit a tan disk with a soft dusty limb, much softer at s = 0.01.
- **No seam is visible anywhere** in the sequences, including the `_top-` / `_top+` pairs.

### The seam, measured

`compare.py`, `veach imgdiff` (mean FLIP; under 0.01 is invisible side by side):

| Pair | Earth s = 1 | Earth s = 0.01 | Mars s = 1 | Mars s = 0.01 |
|---|---|---|---|---|
| 0.5% under the top: LUT vs march, same view | 0.0032 | 0.0035 | 0.0037 | 0.0038 |
| LUT under the top vs march over it (the actual switch) | 0.0042 | 0.0074 | 0.0055 | 0.0090 |
| Ground: LUT vs march (the LUT's own error) | 0.0021 | 0.0025 | 0.0023 | 0.0027 |
| March with 16 steps vs 32, just over the top / orbit | 0.0040 / 0.0011 | 0.0027 / 0.0015 | | |

The switch differs by less than the LUT's own error plus a 1% altitude change. The 99.9th percentiles (0.04 to
0.11) sit on the limb's bright edge, one or two LUT texels wide.

## The scale rule s_a = √s_p, by eye and by number

Vertical optical depth is invariant by construction (`[sp10.scale]`, 1e-9). The images at s = 1 and s = 0.01 are
not:

| Pair (s = 1 vs s = 0.01) | s_a = √s_p (the rule) | s_a = s_p^0.75 | s_a = s_p |
|---|---|---|---|
| Earth, ground, sun 25° up | 0.078 | 0.040 | 0.049 |
| Earth, orbit | 0.025 | 0.010 | **0.0003** |
| Earth, sunset (sun 1.5° up) | **0.434** | 0.318 | 0.156 |
| Transmittance toward that sun (r, g, b) | 0.597, 0.335, 0.103 (s = 1: 0.318, 0.081, 0.006) | 0.456, 0.183, 0.030 | 0.333, 0.087, 0.007 |
| Mars, sunset | 0.347 | | |

![](sp-10/sunset/earth_s1.png) ![](sp-10/sunset/earth_s0.01.png)
![](sp-10/sa/earth_e0.75_sunset.png) ![](sp-10/sa/earth_e1_sunset.png)

*Earth sunsets: s = 1; s = 0.01 with √s; with s_p^0.75; with s_a = s_p.*

- **The cause.** Air mass at the horizon is about √(πR/2H). The rule scales R by s_p and H by √s_p, so the
  horizon air mass scales as s_p^¼: 34.5 at s = 1, 10.9 at s = 0.01.
- **The effect.** At s = 0.01 the sunset sky stays pale blue with a white glow, where s = 1 gives orange and
  purple. Mars at s = 0.01 likewise loses most of its sunset darkening (transmittance 0.0004 → 0.043).
- **At s = 0.1, the default scale** (rendered after the spike, `sunset/earth_s0.1.png`), transmittance toward the
  sun is (0.451, 0.181, 0.029): about what s_p^0.75 gives at s = 0.01. The glow stays warm; the purple band above it
  is mostly gone. Mars at s = 0.1: 0.006.
- **The limb from orbit** gets relatively thicker, as 0063 expects. That reads well (thicker blue rim, softer
  Mars halo).
- **s_a = s_p is exactly invariant** (orbit FLIP 0.0003). That confirms the renderer applies the scale law
  correctly; the ground and sunset residuals with s_a = s_p come from the 2 m camera height and the analytic
  ground, which do not scale.
- **The tradeoff is inherent.** No single s_a keeps both the 10 km layer the ADR wants for clouds and flight and
  Earth-like sunsets: the ratio R/H decides the sunset.

The human-scale haze band (0063's near-field aerial perspective with physical coefficients) was not built or
tested here; S3's "1 km tower" criterion is still open.

## Two suns and an eclipse

| | |
|---|---|
| ![](sp-10/suns/system_sunset.png) | **Two suns from the system query.** `space::reference_system()`'s binary seen from its circumbinary planet at 20° latitude, at the tick star A sets through 4°: A at 4.0° (21,189 lux, 0.11° radius), B at 7.5° (2,872 lux, a 4,400 K tint, 0.07°). Two disks a few degrees apart in one sunset glow, the helmet backlit. Illuminance uses physical distance and an assumed L_B = 0.13 L☉ (the system has no luminosities). |
| ![](sp-10/suns/wide_ground.png) | **Two suns far apart** (A 30° up, B orange at 3°, 110° away in azimuth, 30% of A): one sky-view LUT holds both. The left horizon glows pink under B while the right sky brightens around A. Facing the LUT's azimuth wrap, opposite A, shows no seam ([image](sp-10/suns/wide_wrap.png)). Also at [10 km](sp-10/suns/wide_10km.png), [from orbit](sp-10/suns/wide_orbit.png) (the planet lit from two sides) and on [Mars at s = 0.01](sp-10/suns/wide_mars_s0.01_ground.png). |
| ![](sp-10/eclipse/one_total_ev9.png) | **A total eclipse**, one sun 35° up, a moon at 60 R with 1.15× the sun's angular radius. At the day's exposure ([ev 15](sp-10/eclipse/one_total.png)) the frame is black. At ev 9 the horizon glows orange all around: the air tens of km away is outside the umbra. The per-sample visibility gives this; a global fraction would not. |
| ![](sp-10/eclipse/wide_total_ev11.png) | **One of two suns eclipsed** (ev 11): the sky lit by the low orange sun alone, greyish overhead and warm toward it. At ev 15 ([image](sp-10/eclipse/wide_total.png)) it is nearly black next to the uneclipsed noon. [Partial](sp-10/eclipse/one_partial.png) (separation half the radii's sum): a slightly darker noon sky. |
| ![](sp-10/eclipse/one_total_orbit.png) | **The same eclipse from orbit:** the umbra (about 530 km across) and the penumbra (about 7,600 km) as a dark spot on the lit planet below the camera. It falls out of the per-sample visibility. |

S4 asked for "a double sunset with two shadows". The real-time renderer has no shadows yet, so the helmet is lit by
both suns but shadows were not checked.

## Dense atmosphere (Titan-like): Hillaire's known limit

![](sp-10/titan/titan_s1_ground.png) ![](sp-10/titan/titan_s1_orbit.png)
![](sp-10/titan/titan_s0.01_ground.png) ![](sp-10/titan/titan_s0.01_orbit.png)

- **From orbit at s = 1** a fuzzy ochre ball, which reads as Titan. The ground view is a dim orange-brown sky over
  black ground: direct sun is extinguished (transmittance 0.008, 0.001, 0 at 25°) and the analytic ground gets no
  skylight.
- **At s = 0.01** the rule gives a 36 km atmosphere on a 25.7 km body, so "orbit" at 2 R is barely above the
  haze. The frame is one diffuse glow.
- Both are the limits 0063 lists: Hillaire's multiple-scattering approximation for τ ≈ 4 to 6, and √s for a thick
  atmosphere on a small body. Not tuned further.

## GPU cost

`timings.py`: release build, Apple M5 Max, Metal, 1920 × 1080. Each configuration runs 300 warm-up frames, then 60
frames read one by one through MCP `gpu_timings`; the table is the median, over two interleaved rounds. ms:

| Configuration | sky view | aerial perspective | sky irradiance | shade (whole pass) | transmittance LUT | multiple-scattering LUT |
|---|---|---|---|---|---|---|
| Sky component (ADR 0038 path), ground | 0.018 | 0.009 | 0.091 | 0.066 | | |
| planet, ground, 1 sun (LUT) | 0.026 | 0.012 | 0.095 | 0.091 | | |
| planet, ground, 1 sun, medium rebuilt every frame | 0.025 | 0.012 | 0.096 | 0.091 | 0.006 | 0.192 |
| planet, ground, 2 suns | 0.037 | 0.016 | 0.096 | 0.095 | | |
| planet, ground, 4 suns | 0.058 | 0.023 | 0.096 | 0.105 | | |
| planet, ground, 1 sun + eclipse | 0.045 | 0.017 | 0.095 | 0.091 | | |
| planet, ground, march forced on every pixel, 1 / 2 suns | 0.026 / 0.037 | 0.012 / 0.016 | 0.096 | 1.464 / 2.243 | | |
| planet, just over the top, 1 sun (march; limb fills ~half the frame), 32 / 16 steps | 0.022 / 0.014 | 0.008 | 0.079 | 0.710 / 0.409 | | |
| planet, orbit, 1 sun (march; planet ~17% of the frame), 32 / 16 steps | 0.022 / 0.014 | 0.004 | 0.073 | 0.320 / 0.202 | | |
| planet, orbit, 2 suns | 0.029 | 0.004 | 0.074 | 0.469 | | |
| planet, orbit, 1 sun + eclipse | 0.029 | 0.004 | 0.074 | 0.528 | | |
| Mars s = 0.01, orbit (larger halo) | 0.021 | 0.004 | 0.076 | 0.611 | | |

The other passes (clear 0.005, visibility 0.034, display 0.043) are the same in every row.

- **Per extra sun**, sky view + aerial + shade on the ground: 0.129 → 0.148 → 0.186 ms for 1, 2, 4 suns, so
  **0.019 ms per sun**. 0063's target is under 0.05; the per-pixel march pays about 0.15 ms per extra sun from
  orbit.
- **The eclipse** adds 0.024 ms inside (LUTs) and 0.21 ms in the orbit march: the disk-overlap function is
  evaluated per sample, with its asin and acos.
- **The march** adds about 0.23 ms from orbit and 0.62 ms just over the top (shade pass over the LUT path's 0.091).
  - At 16 steps that drops to 0.11 and 0.32 ms, with FLIP 0.001 to 0.004 against 32 steps.
  - Full-screen it is 1.46 ms per sun. 0063 estimated 0.1 ms on this machine. With 0066's factors (M3 Pro-class
    2.5×, M1-class 6×), the case just over the top (0.62 ms here) becomes about 1.6 and 3.7 ms at 1080p with 32
    steps, or 0.8 and 1.9 ms with 16. The M3 Pro target at 1440p adds about 1.8× more pixels.
- **Sky irradiance costs 0.09 ms in both paths**: a 1 × 1 dispatch where one thread reads 256 texels serially.
  It is existing code and a cheap fix (a parallel reduction).
- **Atmosphere total on the ground, planet path:** 0.13 ms plus its share of shading. That is inside 0063's 0.17
  to 0.25 ms. The multiple-scattering LUT (0.19 ms) is paid only when a body's medium changes.

## Do the decisions hold?

- **ADR 0063, per-body Hillaire with the march from space: holds.**
  - Arbitrary center and local up, composition-driven coefficients, all suns in one absolute-azimuth table, and
    LUT-inside/march-outside with no visible seam (FLIP ≤ 0.009) at s = 1 and 0.01, Earth- and Mars-like.
  - Correction for the ADR's consequences: the march from space costs 0.2 to 0.6 ms on the M5 Max at 32 steps,
    not 0.1. Ship it with an adaptive step count (16 is visually equal here) and consider half resolution for
    full-screen limbs.
- **N suns: holds,** at 0.019 ms per extra sun inside the atmosphere. One sky-view table summed over suns is
  enough; nothing needed a per-sun LUT.
- **Eclipses: hold, with one refinement.** 0063 says per-sun visible fractions come "from the system's queries at
  extraction", i.e. one number per sun. That is right for lighting meshes near the camera, and wrong for the sky:
  from the ground the far air outside the umbra stays lit, and from orbit the umbra is a spot. The occluder has to
  reach the shader as a sphere and be evaluated per sample.
- **ADR 0049's s_a = √s_p: holds for what it promised** (vertical optical depth, the high-sun sky color, a thick
  enough layer at small s), **but not for sunsets.** At s = 0.01 sunsets lose most of their color, which is the
  reopen condition the ADR names. Packet below.
- **ADR 0038's Sky path is untouched:** the real-time and photo-mode goldens pass, and the outputs are
  byte-identical to the baseline except the nondeterministic photo-mode Cornell box.

## Follow-ups (what the real implementation needs)

- **3D textures (VCH-072).** The aerial-perspective atlas works but has the old fixed 32 km depth. Its range must
  follow the body: from orbit, aerial perspective toward terrain is hundreds of km; at s = 0.01 the whole atmosphere
  is 10 km. A 3D froxel volume is the natural home. Clouds need 3D textures regardless.
- **The record:** `space::AtmosphereRecord` with the Rayleigh/aerosol/absorber model is the shape 0063 describes.
  The real one adds:
  - several aerosol and absorber layers, a measured aerosol table, and methane bands;
  - a per-channel Mie phase (Mars's blue aureole);
  - sharing with drag (0059).
- **Per-body LUTs:** cache transmittance and multiple scattering per body (0.006 + 0.19 ms per rebuild). The spike
  has one body.
- **Other bodies' atmospheres** (the moon, a neighbor planet seen from the surface) use the same march around
  another center. The spike draws one planet and an airless moon.
- **The analytic ground needs skylight.** Mars and Titan grounds are too dark without it. Far terrain from orbit
  will be the impostor of 0063 tier 2, which needs the same.
- **Photo mode:** the path tracer still calls the Sky's `sky_radiance`. The planet path was kept out of it
  because its kernel is near Metal's compiler limit (AGENTS.md). Needs a split or a callable before photo mode can
  render a planet.
- **The sky-view LUT's azimuth wrap** uses a clamp sampler. With absolute azimuth the two edges are the same
  direction, so a wrap sampler (or a manual wrap) is the correct fix. Facing the wrap with two suns showed no seam
  at this resolution (`sp-10/suns/wide_wrap.png`); a brighter second sun nearer the wrap could.
- **Human-scale haze band** (0063, 0049): not built; S3's tower criterion still needs a test.
- **Sky irradiance:** 0.09 ms single-thread pass, make it a reduction.
- **Precision:** km floats work at s = 1 from orbit only with the stable quadratic and the inward-ray shadow test.
  The real code should put both in one helper and test them at 1e4 km.

## Review packet: the atmosphere's scale rule after SP-10

- **Decision needed:** keep s_a = √s_p (ADR 0049, confirmed by the owner in ADR 0066) though sunsets at small scale
  lose most of their color, or change the rule.
- **Context.**
  - A creator can compress the universe by a scale factor s_p: s_p = 0.01 makes an Earth-like planet 64 km in
    radius. The atmosphere is compressed less, by s_a = √s_p, so it stays about 10 km thick for clouds and flight.
    Its density is raised to match, so the overhead sky keeps its color.
  - SP-10 rendered it. The overhead sky matches. Sunsets do not. At sunset the light crosses the atmosphere
    sideways, and that path depends on the planet's radius, which shrinks faster than the atmosphere. At
    s_p = 0.01 the setting sun stays nearly white and the sky pale blue; at s_p = 1 the glow is orange under a
    purple sky.
  - Optical depth measures how much air the light crosses. Toward the horizon it is 3.2× smaller at s_p = 0.01.
  - FLIP is a perceptual image difference: 0 is identical, under 0.01 is invisible side by side.
- **Options.**
  - **A. Keep √s.**
    - Pro: 10 km of atmosphere at s = 0.01, as agreed.
    - Con: sunsets FLIP 0.43 from s = 1, blue light 17× more transmitted at 1.5° (0.103 vs 0.006).
    - Locks in pale sunsets at small s.
  - **B. s_a = s_p^0.75.**
    - Pro: about 3 km of atmosphere at s = 0.01; sunsets halfway (blue 0.030, FLIP 0.32).
    - Con: clouds and flight get a thinner layer (Horizon Zero Dawn's cloud layer sits at 1.5 to 4 km).
  - **C. s_a = s_p.**
    - Pro: images exactly scale-invariant (orbit FLIP 0.0003, sunset transmittance equal).
    - Con: a 1 km atmosphere at s = 0.01, the alternative ADR 0049 rejected.
  - **D. Keep √s and add a stylized sunset term.** Extra extinction growing with air mass, applied in the
    transmittance LUT, tuned by eye.
    - Pro: keeps the 10 km layer.
    - Con: not physical; one more tuning knob per composition; the multiple-scattering LUT must agree.
- **Recommendation:** A now, with D as a later stylization task if the owner finds small-scale sunsets lacking.
  - The default scale is s = 0.1, where the horizon path is only 1.8× shorter (s^¼), not 3.2×.
  - The layer thickness affects gameplay; sunset color does not.
  - What would change my mind: the owner judging the s = 0.1 sunset wrong by eye (now rendered: `sunset/earth_s0.1.png`).
- **Risks and reversibility.**
  - s_a is applied only when records become geometry (ADR 0049), so changing it later is cheap until scale locks in
    a universe.
  - D is additive.
  - B or C change flight and cloud heights, which content will depend on.
- **Evidence.**
  - Sunset images: `docs/research/spikes/sp-10/sunset/` and `sp-10/sa/`, shown in the scale section of
    `docs/research/spikes/sp-10-atmosphere.md`.
  - Transmittance toward a sun 1.5° up (red, green, blue), s = 1: (0.318, 0.081, 0.006). At s = 0.01 with
    s_a = √s: (0.597, 0.335, 0.103); s^0.75: (0.456, 0.183, 0.030); s: (0.333, 0.087, 0.007).
  - Orbit FLIP vs s = 1: √s 0.025, s^0.75 0.010, s 0.0003.
- **Questions for the owner.**
  1. At small scale, are paler sunsets acceptable (A), or should we trade atmosphere thickness (B, C) or add a
     stylized term (D)?
  2. The s = 0.1 sunset is rendered (`sunset/earth_s0.1.png`): does it look acceptable at the default scale?
