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
