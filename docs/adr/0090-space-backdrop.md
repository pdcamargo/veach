# 0090: The space backdrop: a baked band and nebulae, stars per pixel, its own exposure

Date: 2026-11-28

## Status

Accepted. Builds on [0063](0063-astronomical-rendering.md) and [0082](0082-amendments-from-sp-14.md) (whose
procedural starfield stays for SP-14's scenes). Spec: [space-backdrop](../specs/space-backdrop/spec.md). VCH-131.

## Context

The owner's favorite view is space, and it has to look alive: stars of many colors and sizes, the galaxy's band,
nebulae. SP-14's starfield is physical: with a sunlit planet in view, auto-exposure sits around EV100 13 to 15 and
stars, 13 to 20 stops darker, vanish; the slice showed black around its planet. A game needs an artistic control
that still looks plausible. The backdrop must be generic engine rendering (the engine never names `space/`,
[0042](0042-space-toolbox-layering.md)), cheap (0.3 ms per frame at 1440p), and each star system's sky must differ
and stay the same on revisits.

## Decision

1. **Data in, pixels out.** `render::Backdrop` (`<veach/render/backdrop.hpp>`) is plain data in
   `ResolvedSky::backdrop`: a seed, the galactic frame's axes in render space (`orientation`, set by the extractor
   every frame, so the sky stays fixed when render space turns with a body-fixed frame), the band (brightness in
   nits, latitude, width, bulge, anticenter, dust), star settings and up to 8 nebulae. `generate_backdrop(seed,
   GalaxyView)` makes one canonically (veach::hash, dmath, [0043](0043-determinism-tiers-and-strict-floating-point.md)).
   `space::system_backdrop(UniverseConfig)` derives the `GalaxyView` from the system address's place in the galaxy
   (a stand-in disk and bulge until the galaxy query exists) and the seed from hash(universe seed,
   "space:backdrop", address). Not a procgen stage: it feeds pixels only, never gameplay.
2. **The band and nebulae are baked; the stars are not.** A compute pass (`shaders/backdrop.slang`) bakes domain-warped
   fractal noise (star clouds, dust lanes that redden what they dim, bulge, emission nebulae with ionization fronts and
   an O III core, reflection and dark nebulae) into a 4096 x 2048 rgba16f lat-long texture **in galactic
   coordinates**, once, when the baked fields change (`Backdrop::bakes_like`). Galactic coordinates put the
   lat-long's pinched poles at the galactic poles, where there is least detail, and keep the band on the equator,
   where texels are square; turning the sky never rebakes. Alpha holds the star density (more stars in star clouds,
   fewer behind dust). The bake is 25 ms on the M5 Max (release), once per system.
3. **Stars per pixel**, so they stay points at any resolution: two cube-face cell grids in galactic coordinates
   (448 and 14 cells per face side, one star at most per cell), each pixel testing its own cell and its three
   nearest. Magnitudes follow log10 N(< m) = 0.45 m to m = 10, illuminance 2.5e-6 lux x 10^(-0.4 m), black-body
   colors (Krystek's Planckian-locus fit) with class shares that move from K and M dwarfs for faint stars to A, B and
   K giants for bright ones. The ~200 brightest have a halo and four spikes along the camera's axes, sized on the sky
   so they end within half a cell.
4. **A pass of its own after shading.** The shade pass marks the sky pixels (radiance alpha 2); `space backdrop`
   (`shaders/backdrop_composite.slang`) adds the backdrop through the air (`sky_to_space`: 1 with no sky, the
   transmittance LUT through an atmosphere, 0 through the ground or the occluder) and writes alpha = the share of the
   pixel's luminance that is the sky's. The exposure histogram meters luminance x alpha: **the backdrop never moves
   auto-exposure.** Without this, the boosted backdrop pulled the exposure up and blew out the planet.
5. **Its own exposure, a creator setting.** `SpaceBackdrop { seed, visible, exposure_compensation = 4,
   exposure_follow = 0.1 }` (any one entity, Scriptable): the backdrop is shown `exposure_compensation` stops above
   physical at EV100 -4 (deep space) and dims with the camera's exposure only by `exposure_follow` (1 is physical).
   From deep space to a sunlit planet (about 17 stops) it dims by about 1.7 stops: alive, and still darker next to
   the planet. Where the sky itself is bright the boost fades back to physical (gain = 1 + (g - 1) exp(-30 L), L the
   sky's exposed luminance), so a day sky stays starless and the limb's glow hides what is behind it.
6. **An empty scene with a backdrop renders.** Deep space is a scene of its own (no meshes), so the scene renderer
   draws when the sky has a visible backdrop; photo mode then falls back to real time.

Measured on the M5 Max, release, 2560 x 1440: the `space backdrop` pass 0.29 ms in the slice's orbit view and
0.30 ms in deep space (every pixel sky); the texture, `sky_to_space` and gain 0.12 ms, the faint stars 0.08 to
0.13 ms, the bright ones 0.07 ms after their 3 x 3 search became 2 x 2 (0.36 ms before).

## Consequences

- Every system has its own sky from data the toolbox controls; the look is tunable without code (component, data).
- 64 MB of GPU memory while a backdrop is drawn, and a 25 ms bake when the system changes. For warps between systems
  the bake should be spread over frames into a second texture (backlog). With cube maps (VCH-072) the bake can move
  to a cube map at the same texel density for less memory.
- Photo mode does not see the backdrop yet (the path tracer is near Metal's compiler limit, as for SP-14's stars).
- The radiance target's alpha now carries the metered share; any new pass that writes the HDR target before metering
  must write alpha 1 (or its own share).
- The budget is on its limit at 0.30 ms on the M5 Max; slower GPUs will need fewer field cells or a half-rate
  star pass. Revisit with the [0060](0060-gpu-driven-visibility-buffer.md) renderer.

## Alternatives considered

- **Everything per pixel** (noise in the full-screen pass): the band's and nebulae's fractal noise is 60 to 100
  noise evaluations per pixel, far over 0.3 ms at 1440p; and it would be the same every frame.
- **Everything baked, stars included:** stars smaller than a texel blur into blobs or alias; a texture fine enough
  for 1440p points (~16k wide) is far too big.
- **Stars as point sprites:** independent of resolution and cheap, but needs additive blending, which the RHI does
  not have yet (VCH-098).
- **A cube map now:** the RHI has no cube maps (VCH-072 is adding them); a lat-long in galactic coordinates puts its
  only weakness, the poles, where the sky is emptiest.
- **One fixed boost in stops:** right either next to a planet or in deep space, never both (17 stops apart).
  **Physical only (follow = 1):** the black sky the owner does not want.
- **Metering the backdrop:** a boosted backdrop then sets the exposure; the planet blew out (measured).
