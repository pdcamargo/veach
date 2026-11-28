# Space backdrop: screenshots

VCH-133, [spec](../../../specs/space-backdrop/spec.md), [ADR 0091](../../../adr/0091-space-backdrop.md). Metal on the
M5 Max, 2560 x 1440, release build, default creator settings in the slice (`SpaceBackdrop { exposure_compensation =
4, exposure_follow = 0.1 }`), the same values in the lab's deep-space script.

| Image | What |
|---|---|
| [before_orbit_s0.01_seed42.png](before_orbit_s0.01_seed42.png) | The slice in orbit before: SP-14's physical starfield, invisible next to the lit planet. |
| [orbit_s0.01_seed42.png](orbit_s0.01_seed42.png) | The same view now, s = 0.01: band, a large emission nebula, stars with spikes. Auto-exposure unchanged (the planet is exposed exactly as before). |
| [orbit_s1_seed42.png](orbit_s1_seed42.png) | s = 1, same seed: the same sky (the backdrop does not depend on the scale). |
| [orbit_s0.01_seed7.png](orbit_s0.01_seed7.png), [orbit_s0.01_seed2026.png](orbit_s0.01_seed2026.png) | Other universes, same system address: other skies. |
| [deep_space_seed7.png](deep_space_seed7.png) | Deep space, no planet (`apps/lab/scripts/space_backdrop.luau`, EV100 -4). |
| [deep_space_seed7_wide_center.png](deep_space_seed7_wide_center.png), [deep_space_seed7_wide_nebulae.png](deep_space_seed7_wide_nebulae.png) | The same sky at 100 degrees: toward the galactic center, and along the band with three emission nebulae. |
| [deep_space_seed3.png](deep_space_seed3.png), [deep_space_seed11.png](deep_space_seed11.png) | Other seeds: dust lanes, a blue reflection nebula, bright stars of different colors. |

Reproduce: `build/release/apps/lab/veach-lab --rhi metal --headless --frames 3 --script scripts/space_backdrop.luau
--screenshot out.png` (from `apps/lab`), and `build/release/apps/slice/veach-slice --script scripts/slice_s0.01.luau
--rhi metal --headless --frames 60 --screenshot out.png` (from `apps/slice`).

GPU time (release, 1440p): `space backdrop` 0.29 ms in the orbit view, 0.30 ms in deep space; `space backdrop bake`
25 ms once per system.

## A lit sky hides the boost

VCH-137, [rule](../../../specs/space-backdrop/spec.md#the-sky-in-front-hides-the-boost). Standing or flying low in
daylight, the backdrop's stars showed through the blue sky: next to a lit scene the creator's boost is about 20 stops,
and the first fade (by the sky's exposed luminance) took back only 2 to 4 of them, because auto-exposure keeps a day
sky dim in the image. The boost now fades by how much the sky outshines the backdrop in the world (`v`) where it also
shows against it in the image (`x`). Same release build, M5 Max, 2560 x 1440, slice autopilot with
`VEACH_SLICE_SHOTS` (the demo now adds dusk and twilight views); the lab images are 480 x 270 at EV100 15.

| View | Before | After |
|---|---|---|
| s = 1, 200 m, midday | [day_s1_haze_200m_before.png](day_s1_haze_200m_before.png): stars all over the blue | [day_s1_haze_200m_after.png](day_s1_haze_200m_after.png): none |
| s = 0.01, 200 m, midday | [day_s0.01_haze_200m_before.png](day_s0.01_haze_200m_before.png) | [day_s0.01_haze_200m_after.png](day_s0.01_haze_200m_after.png) |
| Lab, noon, EV100 15 (`scripts/space_backdrop_day.luau`) | [day_lab_noon_before.png](day_lab_noon_before.png) | [day_lab_noon_after.png](day_lab_noon_after.png); at midnight, same camera: [day_lab_midnight_after.png](day_lab_midnight_after.png) |
| s = 1, top of the atmosphere | [day_s1_atmosphere_top_before.png](day_s1_atmosphere_top_before.png) | [day_s1_atmosphere_top_after.png](day_s1_atmosphere_top_after.png): the same |
| s = 1, twilight, sun 10 degrees down | [day_s1_twilight_sun_-10_before.png](day_s1_twilight_sun_-10_before.png) | [day_s1_twilight_sun_-10.png](day_s1_twilight_sun_-10.png): more stars in the lower sky |

After only: the evening at s = 1, sun 2 degrees up [day_s1_sunset_sun_2.png](day_s1_sunset_sun_2.png) (no stars),
3 down [day_s1_dusk_sun_-3.png](day_s1_dusk_sun_-3.png) (none yet), 6 down
[day_s1_twilight_sun_-6.png](day_s1_twilight_sun_-6.png) (the brightest few), 10 down (the band and many stars);
half a radius up [day_s1_high_after.png](day_s1_high_after.png) and in orbit at s = 0.01
[day_s0.01_orbit_after.png](day_s0.01_orbit_after.png), compare [orbit_s0.01_seed42.png](orbit_s0.01_seed42.png).

- **Orbit, high and deep space are unchanged.** Against the same build before the fix, mean FLIP 0.0001 (orbit, s =
  1), 0.001 (orbit, s = 0.01), 0.0002 and 0.001 (half a radius up), at the run-to-run noise of views with no sky
  (0.001 on foot). Deep space (`scripts/space_backdrop.luau`) is byte-identical, and so is the golden.
- **Two rules failed on the way.** Dividing the boost by `1 + v` alone made midday starless but blacked out a band of
  sky over the limb from orbit and the top of the atmosphere: up there the faint air is physically 1e3 to 1e6 times
  the band, yet black at the sunlit planet's exposure. Gating `v` by a power of `x` left a thinner dark rim around the
  limb at s = 0.01, where the air is thick. The gate in stops (no fade while the sky is under half the boosted
  backdrop in the image, the full physical fade from twice it) leaves both views as they were.
- **GPU cost.** `space backdrop` in deep space at 1440p, median of 60 frames: 0.300 ms before, 0.304 ms after (the
  gate returns early under a black sky).
- **Seen on the way, not changed here:** at 10 degrees below the horizon the slice's sky is drawn in hard concentric
  bands, before the fix as after (the atmosphere's twilight, VCH-136).
