# Space backdrop

**Status:** Implemented
**Area:** render, space
**Owner:** dev lead

## Summary

What lies beyond every body: a dense field of stars with black-body colors and a realistic spread of magnitudes, a
galactic band with star clouds, a bulge and dust lanes, and emission, reflection and dark nebulae. The engine draws
it behind the sky from data; the space toolbox derives that data from the universe seed and where the system is in
the galaxy, so each system's sky differs and stays the same on every revisit. It is the owner's favorite view: space
must look alive, including with a sunlit planet in frame.

## Motivation

Before this, the sky beyond a planet was SP-14's starfield (`ResolvedSky::stars`, ADR 0082): physically dim, it
vanished next to a sunlit planet under auto-exposure, and the slice showed a black sky around its planet. VCH-133.

## Requirements

- The engine MUST draw a backdrop given as data (`render::Backdrop`): seed, galactic frame, band brightness,
  latitude, width, bulge, anticenter, dust, star density, magnitude scale, saturation, up to 8 nebulae (direction,
  radius, kind, colors, brightness, shape). It MUST NOT name `space/` (ADR 0042).
- Stars MUST be drawn per pixel, crisp at any resolution, with colors from black-body temperatures (faint stars
  mostly K and M, bright stars mostly A, B and K giants), counts following log10 N(< m) = 0.45 m, illuminance
  2.5e-6 lux x 10^(-0.4 m) (ADR 0063); the brightest MUST get a halo and diffraction spikes.
- The band and nebulae MUST be baked once into a texture when their data change, never every frame.
- The backdrop MUST cost at most 0.32 ms of GPU time per frame at 2560 x 1440 on the M5 Max (0.304 ms with the lit-sky rule).
- The backdrop MUST be exposed on its own (`SpaceBackdrop.exposure_compensation`, `exposure_follow`), MUST NOT move
  auto-exposure, and MUST fade back to physical where the sky itself is bright (no stars in a day sky), by the
  rule below.
- `space/` MUST derive a system's backdrop canonically (veach::hash, dmath) from the universe seed and the system
  address, independent of the scale, and UniverseModule MUST give it to the renderer turned into render space.

### The sky in front hides the boost

The creator's boost (the gain `g` above physical, from `exposure_compensation` and `exposure_follow`) is taken back
per pixel where the sky in front of the backdrop outshines it:

    gain = 1 + (g - 1) / (1 + v)^w,   v = L_sky / L_backdrop,   x = v / g,
    w = smoothstep(0, 1, (log2 x + 1) / 2)          (0 for x <= 0.5, 1 for x >= 2)

`L_sky` is the sky's own radiance luminance along the pixel's ray, physical (nits, the exposure divided out): the
air's in-scattered light, the limb's glow, a sun or moon disk. `L_backdrop` is the backdrop's: its band's brightest,
`Backdrop::band_brightness` (the Milky Way is about 5e-4 nits), at least 1e-4 nits. `v` says how much brighter the
sky is in the world, `x` how much brighter it is in the image than the boosted backdrop. `w` lets the sky hide the
boost only where it shows against it: where the sky is twice the boosted backdrop in the image or more, the backdrop
is physical (divided by 1 + v); under half, the boost is untouched. So:

- under a black sky (deep space, past the limb from orbit, the night side, a moonless night) the boost is whole;
- under a day sky (1e3 to 1e4 nits: `v` about 1e7, `x` about 5) the boost is gone and the backdrop is physical,
  millions of times darker than the sky: no stars;
- the air around a planet's limb seen from orbit or from the top of the atmosphere is physically far brighter than
  the band (`v` up to 1e6) but dimmer than the boosted band at the sunlit planet's exposure: the boost stays, and
  space around the limb looks as it did before the rule;
- in twilight the boost comes back as the sky darkens, and a star shows once it outshines the sky in its pixel, so the
  brightest come out first.

The physical ratio alone fails from above: dividing by 1 + v, or gating it by any power of `x`, blacked out a ring
of sky over the limb where the air is physically bright but too faint to see (`v = x g`, and `g` is about 2e6 in
orbit). The image ratio alone fails by day: auto-exposure raises `g` with the sky (0.9 stops per stop), so a midday
sky is only about 5 times the boosted band in the image; the first version, a fade by the exposed sky (`exp(-30 L)`,
ADR 0091 rule 5, which this rule replaces), took back only 2 to 4 of about 20 stops and the stars showed through.
The rule costs a few instructions per sky pixel in the `space backdrop` pass (the sky's radiance is already there).

## API sketch

```cpp
// engine: data, a canonical generator, and the creator's settings
render::Backdrop b = render::generate_backdrop(seed, render::galaxy_view_from_seed(seed));
out.sky.backdrop = b;                       // an extractor; b.orientation = galactic axes in render space
struct SpaceBackdrop { uint32_t seed; bool visible; float exposure_compensation; float exposure_follow; };

// space: a system's sky
render::Backdrop sky = veach::space::system_backdrop(config);   // UniverseConfig: seed + system address
render::GalaxyView view = veach::space::galaxy_view(config);
```

```lua
camera:set(c.SpaceBackdrop, { seed = 7, visible = true, exposure_compensation = 4, exposure_follow = 0.1 })
```

## Acceptance criteria

- **backdrop.AC1** — `generate_backdrop` is canonical: equal output for equal input, committed hashes for two
  seeds, a distinct sky for each of 32 seeds, unit nebula directions, a rotation for the galactic frame.
- **backdrop.AC2** — `blackbody_color` is white at 6500 K, red > green > blue at 3000 K, the reverse at 15,000 K, at
  unit luminance.
- **backdrop.AC3** — turning a backdrop or changing its star settings does not rebake it; its band or seed does.
- **backdrop.AC4** — `system_backdrop` is the same on every call and at every scale, differs by seed and by system,
  and its hashes for the slice's system and an Earth-like one are committed.
- **backdrop.AC5** — nearer the galactic center the band is brighter, the bulge bigger and the anticenter brighter;
  far above the plane the band is fainter, wider, lower and has fewer nebulae.
- **backdrop.AC6** — the slice on the null RHI bakes once, draws the `space backdrop` pass every frame, logs no
  error, and the renderer's backdrop bakes like `system_backdrop`.
- **backdrop.AC7** — Metal golden `tests/golden/space_backdrop.png` (`scripts/space_backdrop_small.luau`, 480 x 270);
  on DX12 (WARP) the same script runs without an error.
- **backdrop.AC8** — measured: the `space backdrop` pass at most 0.32 ms at 1440p (release, M5 Max), in the slice's
  orbit view and in deep space. Recorded in [ADR 0091](../../adr/0091-space-backdrop.md).
- **backdrop.AC9** — under a midday sky (`scripts/space_backdrop_day.luau`, the physical sky at noon, EV100 15) the
  backdrop at its default settings differs from the physical one (0 stops, follow 1) by at most one 8-bit step in
  any channel of any pixel; at midnight, same camera, by at least 100 (`tests/mcp/test_mcp_lab.py backdrop_sky`).

## Out of scope

- Photo mode's escaped rays do not see the backdrop yet (the path tracer's kernel is near Metal's compiler limit).
- A real galaxy query (where the stars are, which ones are near): the toolbox's galaxy model here is a stand-in.
- Baking into a cube map once the RHI has them (VCH-072), and spreading the bake over frames for warps (VCH-133
  follow-ups in the backlog).

## Open questions

None.

## Links

- ADRs: [0090](../../adr/0091-space-backdrop.md), [0082](../../adr/0082-amendments-from-sp-14.md),
  [0063](../../adr/0063-astronomical-rendering.md), [0042](../../adr/0042-space-toolbox-layering.md),
  [0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md), [0050](../../adr/0050-universe-addresses-and-seeds.md)
- Backlog: VCH-133, VCH-137
- Guides: [rendering](../../guides/rendering.md), [space realization](../../guides/space-realization.md)
- Screenshots: [research/spikes/space-backdrop](../../research/spikes/space-backdrop/README.md) (the day-sky fade:
  [before and after](../../research/spikes/space-backdrop/README.md#a-lit-sky-hides-the-boost))
