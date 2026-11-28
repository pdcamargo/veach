# SP-15: the first slice (VCH-091, built as VCH-115)

The milestone the research phase pointed at: "A seed-generated star system with a rotating, tilted planet seen from
orbit, an atmosphere from its composition and the correct local time where you stand; done when you fly down with no
loading screen, land and dig a hole, with the same code at s = 0.01 and s = 1 switched by configuration."
([spec](../../specs/first-slice/spec.md), [ADR 0088](../../adr/0088-the-realization-layer-and-the-first-slice.md),
[guide](../../guides/space-realization.md)). All numbers are from the Apple M5 Max, 2026-11-28.

**Verdict: done at both scales.** `veach-slice` realizes universe seed 42, system `S:1000400100008000` (an F star and
eight planets; `b1` is an Earth-sized temperate planet with a 35.3 h day, tilted 38.6°, with a moon), spawns a craft in
orbit one radius up, and flies it through the cruise drive, the atmosphere-top frame handoff and the surface physics
scene to a hover at 50 m and a landing; the pilot gets out, walks 8 m, digs three times and stands in the hole. The
same code runs at s = 0.01 and s = 1; the startup scripts differ in one number. Both runs are recorded through
`--record` from gamepad events and replay bit for bit; their hashes are committed, so the macOS gate and Windows CI
check them.

![Orbit at s = 0.01: the planet, its atmosphere's limb, the terminator and the moon](sp-15/s0.01_1_orbit.png)

## What was built

| Piece | Where |
|---|---|
| Atmosphere stage `space:body.atmosphere` (after B1) | `space/gen/{include/veach/space/gen,src}/atmosphere.*` |
| Realization: seed + settings → `StarSystem` (stages, scale, orbit tree, atmosphere, terrain field) | `space/include/veach/space/realization.hpp`, `space/src/realization.cpp`, `terrain.cpp` |
| `UniverseModule`: frames on rails, clock, terrain bodies, gravity hook, sky extractor | `space/include/veach/space/universe.hpp`, `space/src/universe.cpp` |
| Flight policy: governor, handoff bands, assisted flight + cruise, transported heading | `space/include/veach/space/flight.hpp`, `space/src/flight.cpp` |
| The slice app: craft, pilot, dig, camera, readout, autopilot | `apps/slice/` (`veach-slice`, scripts `slice_s0.01.luau`, `slice.luau`, `slice_s1.luau`) |
| Tests `[slice.AC1]`–`[slice.AC6]` | `tests/space_realization_tests.cpp`, `tests/slice_tests.cpp` |
| Engine fixes found on the way | `engine/physics/src/module.cpp`, `world.cpp`, `jobs.cpp`; `engine/voxel/src/terrain.cpp`, `density.cpp` (ADR 0088 decisions 5 and 6) |

## The two scales

| | s = 0.01 | s = 1 |
|---|---|---|
| Stage hashes (G0, S1, B1, atmosphere) | `14b9ee7757e680f6 de0d92a50f020abf 1e1f3aa3566bb95e 0614c8b54778c448` | the same |
| Realized geometry hash | `0x2d9fc74583c7539b` | `0xb29395ad55c79549` |
| Home radius, g, sidereal day, tilt | 63.7 km, 9.82 m/s², 35.31 h, 38.6° | 6,371 km, 9.82 m/s², 35.31 h, 38.6° |
| Atmosphere top (100 km-ish × √s) | 9,915 m | 99,154 m |
| Landing site (flattest sunlit candidate of 60) | 40°N 0°E, 397 m up, local solar time 13:30, sun 71.7° up | 30°N 90°W, 1,666 m up, 07:30, sun 34.3° up |
| Orbit at spawn (one radius up, circular) | 63.7 km, 559 m/s | 6,371 km, 5,593 m/s |
| Inertial → body-fixed handoff | 9,912 m, 631 m/s, \|Δv\| 0, \|Δx\| 0 | 99,141 m, 1,995 m/s, \|Δv\| 0, \|Δx\| 0 |
| Free flight → surface scene (500 m above the ground) | 112.1 m/s, \|Δv\| 1.0e-6 m/s | 112.1 m/s, \|Δv\| 2.6e-6 m/s |
| Touchdown (engine off) | 75 s after spawn | 168 s after spawn |
| Recorded landing hash (`[slice.AC5]`) | `0x8163fa461afa85ab` | `0x1ee27804b67dadbf` |
| Recorded dig hash (`[slice.AC6]`) | `0x5420285180e141c7` | `0x9acac2bc2caeedb8` |
| Feet below the undug ground at the end | 3.7 m | 3.6 m |
| Debug test (record + replay), M5 Max | 49 s | 64 s |

The landing sites differ because the terrain differs by design: the planetary band's relief scales with √s_p and the
meter band does not, so the flattest candidate moves (at the default s = 0.1 it is 40°N 60°W at 09:30) (ADR 0049: the same seed gives the same records and coarse
fields at any s, not the same trees). Continents and mountain ranges are the same shapes at both scales (the orbit
screenshots). The |Δv| at the surface-scene entry is Jolt's float32 velocity, as SP-13 measured (2.8e-6 m/s there).

Descent time is bound by the thrusters, not the governor: the cruise drive closes from one radius to the atmosphere
top in 18 s (s = 0.01) and 39 s (s = 1), then the craft brakes at 20 m/s² from the entry speed √(2 a h_top), which
grows with √s, so the atmosphere, the 5 s hover and the landing take 54 s and 126 s.

## Screenshots, 2560 × 1440

| | s = 0.01 | s = 1 |
|---|---|---|
| Orbit | [orbit](sp-15/s0.01_1_orbit.png) | [orbit](sp-15/s1_1_orbit.png) |
| Descent, half a radius up | [high](sp-15/s0.01_2_descent_high.png) | [high](sp-15/s1_2_descent_high.png) |
| In the atmosphere (30 % of the top) | [3 km](sp-15/s0.01_3_in_atmosphere.png) | [30 km](sp-15/s1_3_in_atmosphere.png) |
| Near the ground (200 m) | [200 m](sp-15/s0.01_4_near_ground.png) | [200 m](sp-15/s1_4_near_ground.png) |
| Landed | [landed](sp-15/s0.01_5_landed.png) | [landed](sp-15/s1_5_landed.png) |
| Dug | [dig](sp-15/s0.01_6_dig.png) | [dig](sp-15/s1_6_dig.png) |
| In the hole | [hole](sp-15/s0.01_7_in_the_hole.png) | [hole](sp-15/s1_7_in_the_hole.png) |

![In the atmosphere at s = 0.01, 3 km up: the curvature of a 64 km planet](sp-15/s0.01_3_in_atmosphere.png)
![The pilot beside the hole at s = 1](sp-15/s1_6_dig.png)

What they show and what they don't: the atmosphere's limb is thick at s = 0.01 (15.6 % of the radius) and a thin line
at s = 1 (1.6 %), as ADR 0049 derives. The tilt is visible only as the terminator's slant; the camera's up is the
orbit's normal. The starfield is drawn but invisible next to a sunlit planet under auto-exposure, which is physically
right (SP-14's deep-space shots show it). The interim renderer has no shadows (ADR 0040), so the hole reads by its
shading only; its rim shows Transvoxel's facets at 0.5 m cells.

## Frame times at 2560 × 1440 (release, Metal, M5 Max)

`VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=dir veach-slice --rhi metal --headless`, logged at each milestone. CPU is
the main loop's wall time per frame (headless, unpaced: everything the frame waited for), GPU the device's frame
time; both smoothed over about 10 frames. Visibility and shade are the largest passes, from the milestone frame.

| Milestone | s = 0.01 CPU / GPU ms | visibility / shade | s = 1 CPU / GPU ms | visibility / shade |
|---|---|---|---|---|
| Orbit | 0.83 / 0.83 | 0.07 / 0.51 | 0.73 / 0.73 | 0.07 / 0.40 |
| Half a radius up | 1.69 / 1.29 | 0.17 / 0.85 | 2.64 / 0.88 | 0.29 / 0.45 |
| Atmosphere top | **15.1** / 2.02 | 0.29 / 1.56 | 1.87 / 1.54 | 0.50 / 0.75 |
| In the atmosphere | **14.9** / 1.92 | 0.90 / 0.77 | 3.14 / 2.08 | 0.96 / 0.61 |
| 2 km | 3.97 / 1.17 | 0.47 / 0.38 | 4.67 / 3.00 | 1.75 / 0.84 |
| 200 m (collider region arrives) | **70.1** / 4.07 | 2.84 / 1.77 | **15.2** / 3.88 | 1.52 / 0.61 |
| Hover, 50 m | **22.4** / 3.59 | 1.25 / 0.61 | 7.08 / 4.32 | 3.28 / 0.94 |
| Landed | 5.72 / 3.25 | 1.57 / 0.75 | 8.09 / 3.96 | 2.37 / 0.97 |
| Digging | **45.2** / 3.73 | 3.57 / 1.86 | 11.6 / 5.34 | 3.41 / 1.25 |
| In the hole | 6.14 / 3.70 | 2.31 / 0.92 | 7.29 / 4.96 | 3.13 / 1.19 |

- **GPU stays under 5.4 ms** everywhere at 1440p: the interim renderer's visibility pass grows with the terrain's
  triangles near the ground.
- **The CPU spikes are terrain work on the main thread**, not rendering: the descent through s = 0.01's thick
  atmosphere at 600 m/s streams the most chunks per second relative to a small planet; at 200 m and while digging the
  first LOD0 collider region is classified, sampled and meshed synchronously (`update_colliders` waits for its jobs,
  ADR 0058: the simulation stalls rather than differs). Steady state is 4-9 ms. Spreading the collider region over
  frames ahead of need (a larger reach at low speed, prefetch below 1 km) is a follow-up.
- **Not measured here: the M3 Pro design target (ADR 0066) or an M1.** The synthesis's ×2.5 (M3 Pro) and ×6 (M1)
  scaling of M5 Max GPU time (§7, estimates) would put the worst GPU frame near 13 ms and 32 ms; the CPU hitches would
  scale too. SP-21 (VCH-097) measures real hardware.

## Engine costs a whole flight exposed (ADR 0088 decisions 5 and 6)

- **Physics never stepped.** `PhysicsModule`'s step was a system with query terms (`PhysicsBody`, `Transform`); Flecs
  does not run a system whose own query matches nothing, so bodies made through `physics_world(world)` sat still. The
  step system now has no terms.
- **LOD0 colliders reclassified every chunk with a 6-level split on every call.** Near the surface the bound proves
  nothing (the meter band's octaves count by their range), so each chunk cost the whole 8⁶-node split: a Debug slice
  sat for minutes at 50 m. Shallow classification (one split), cached per chunk, and rings rebuilt only when an input
  mesh changed bring a Debug landing to 2-3 ms per frame. The cooked shapes are the same; `[voxel.AC8]`'s ring hash
  changed because neighbors without triangles no longer enter it.
- **A one-craft scene spent most of its step waking workers.** Scenes under 256 Jolt bodies now step on the calling
  thread. SP-11's, SP-12's, SP-13's and the seams sweep's committed hashes are unchanged.

## Determinism

The realization, the flight model, the autopilot, the voxel colliders and Jolt are canonical or cross-platform
deterministic; the slice's state hash covers the universe and physics ticks, the craft's scene, frame, origin and Jolt
state, the headings, the pilot, the edit store's hash and every installed collider's shape hash. `[slice.AC5]`
records the run through the engine's input path (five gamepad axes through their dead zones, two buttons) with
`--record` and replays it with `--replay`: the per-frame state hash never diverges and the final hashes match. The
local solar time and the sky are display values (`asin`, `atan2`) and stay out of the hash.

## Follow-ups

- Collider prefetch so the first collider region doesn't stall the frame (the 70 ms and 45 ms hitches above).
- Re-entering the craft and flying off; landing on the moon; other planets drawn (they are frames on rails without
  terrain); SOI handoffs between bodies; entry drag and heating (ADR 0059).
- Physics warp and rails warp with live scenes (the clock is pinned to one tick per step).
- `universe.json` (ADR 0055) as the genesis file instead of the startup script's component.
- Debug runtime: the two end-to-end tests take 49 s and 64 s; most of it is the procgen interpreter at -O0 sampling
  collider chunks and the null-RHI render loop.

## Reproduce

```sh
build/debug/tests/veach-tests -tc="*slice*"                              # AC1-AC6, both scales
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out build/release/apps/slice/veach-slice \
  --script scripts/slice_s0.01.luau --rhi metal --headless --frames 12000  # screenshots and frame times
build/debug/apps/slice/veach-slice --script scripts/slice_s1.luau --window # fly it
```
