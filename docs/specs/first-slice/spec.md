# The first slice: the realization layer and the slice app

**Status:** Implemented
**Area:** space, apps
**Owner:** dev lead (the done criterion is the owner's, VCH-091)

## Summary

A universe seed and the creator's scale settings become a star system to fly in: `space/`'s realization layer runs
the generation stages, applies the scale factors and builds the system as reference frames on rails, with an
atmosphere and a voxel terrain per body, gravity, the handoff bands and the travel governor. `veach-slice`, the first
app shaped like a game, spawns a craft in orbit of the generated planet, flies it down without a loading screen,
lands, lets the pilot out and digs a hole, with the same code at s = 0.01 and s = 1. ([ADR 0049](../../adr/0049-scale-model.md),
[0059](../../adr/0059-gravity-flight-travel.md), [0080](../../adr/0080-amendments-from-sp-13.md),
[0088](../../adr/0088-the-realization-layer-and-the-first-slice.md))

## Motivation

VCH-091 (SP-15) is the milestone the research phase pointed at: "A seed-generated star system with a rotating, tilted
planet seen from orbit, an atmosphere from its composition and the correct local time where you stand; done when you
fly down with no loading screen, land and dig a hole, with the same code at s = 0.01 and s = 1 switched by
configuration." Its parts exist on their own: frames (VCH-111), procgen (VCH-112), physics (VCH-113), voxel terrain
(VCH-114), SP-04's orbits, SP-05's stages, SP-10's atmosphere, SP-14's exposure and starfield. VCH-115 assembles them.
Backlog: VCH-115, VCH-091.

## Requirements

**Realization (`veach::space::realize`, `space/include/veach/space/realization.hpp`)**

- MUST take a `UniverseConfig` (seed, system address, s_p, s_o, the atmosphere and relief exponents, gravity and day
  multipliers, habitability bias) and refuse out-of-range settings with `universe.settings_invalid` (s_p in
  [0.01, 1], s_o in [s_p, 1], [ADR 0068](../../adr/0068-amendments-from-sp-05.md)) and a bad address with
  `universe.system_invalid`.
- MUST run the stages G0, S1, B1 and `space:body.atmosphere` on `veach::procgen`'s stage runtime with no scale input,
  so their record hashes are the same at every s (ADR 0049), and apply the scale only afterwards: radii and moon
  orbits × s_p, planet orbits and the star × s_o, atmosphere heights × s_a = s_p^0.5 (coefficients × 1/s_a), relief ×
  s_p^0.5, every GM = g R², periods from those GMs, sidereal days physical (a tidally locked body's day is its
  orbit).
- MUST build the system as an orbit tree (`space::System`, ADR 0048) and check each day length (ADR 0049's 30 % warn
  and 70 % refuse ratios).
- MUST derive each body's atmosphere record from its composition (`space::atmosphere_from`), and each solid body's
  terrain density from a field program with the planetary band in planet meters and the meter band unscaled.
- MUST be canonical (`+ − × ÷ √`, `veach::dmath`, hash streams) wherever it feeds simulation.

**The universe in a world (`veach::space::UniverseModule`, `universe.hpp`)**

- MUST realize the system when a `Universe` component is set (from a startup script or MCP), once; a later set with
  other settings is `universe.settings_invalid` and changes nothing (scale is genesis, ADR 0055).
- MUST build a frame tree: an inertial system frame; per body an inertial center frame on rails and a rotating,
  radial-up body-fixed child with its spin and axial tilt; and write every frame's pose for the end of each fixed step
  in PostSimulation from the int64 tick (ADR 0085 rules 4 and 5), advancing a `UniverseClock` one tick per step.
- MUST answer the physics gravity hook for both frames of every body with that body's g R² (ADR 0059's dominant
  body).
- MUST give the home planet and its moons a voxel `TerrainBody` in their body-fixed frames (TerrainModule streams and
  draws them).
- MUST give the renderer the sky of the nearest body with an atmosphere (`ResolvedSky::planet`), the star from the
  orbit tree at its physical illuminance (illuminance uses physical distance, ADR 0049), through the atmosphere's
  transmittance as the directional light, the starfield and auto-exposure (ADR 0063, 0082).

**Flight policy (`space/include/veach/space/flight.hpp`)**

- MUST cap speed with ADR 0059's governor, `max(v_floor, d / τ)`, τ = 5 s near bodies and 7 s in cruise, v_floor =
  100 m/s, d the height above the ground, identical at every scale.
- MUST hand off frames and scenes at ADR 0080's bands: body-fixed below the atmosphere top (100 km × s_a for airless
  bodies), inertial above 1.1 × it; the surface physics scene below 500 m above the ground, free flight above 750 m;
  and slow a craft entering the surface scene to at most 150 m/s (no continuous collision detection yet).
- MUST fly the assisted Newtonian model (stick input becomes a target velocity within the cap, the thrusters close on
  it within 40 m/s² and counter gravity) below the atmosphere top, and a kinematic cruise drive above it whose limit
  falls to the speed the thrusters can brake from over the atmosphere's depth (ADR 0088).
- MUST take the craft's horizontal axes from its own parallel-transported heading, never from north or the spin axis
  (the seams sweep's 0/0 at the pole).

**The slice (`apps/slice`, `veach-slice`)**

- MUST spawn a craft in a circular orbit one radius above a landing site on the home planet, chosen as the flattest
  of a fixed set of candidate points that have the sun at least 20° up at the start (any, if none has), in the
  planet's inertial frame.
- MUST read every control from analog actions and buttons in the fixed step, so `--record` and `--replay` repeat a
  run exactly; MUST stream nothing during the flight that the simulation waits for (no loading screen).
- MUST let the pilot get out of the landed craft as a character in the surface scene, walk with a transported
  heading, and dig with brush ops whose colliders are swapped under the pilot (ADR 0077's install at t + 2).
- MUST publish a `SliceReadout` (mode, frame, scene, altitude, height, speed, cap, local solar time and the sun's
  elevation where the subject is, latitude, longitude, scale, the canonical state hash) every step and log it.
- MUST switch scale by configuration only: `scripts/slice_s0.01.luau`, `scripts/slice.luau` (0.1) and
  `scripts/slice_s1.luau` differ in the `scale` they set.

## API sketch

```lua
-- scripts/slice_s0.01.luau
veach.spawn("universe"):set(c.Universe, { seed = "42", system = "S:1000400100008000", scale = 0.01, start_tick = 0 })
```

```cpp
auto system = veach::space::realize({.seed = 42, .system = address, .scale = 0.01});  // Result<StarSystem>
const veach::space::RealizedBody& home = *system->home();
auto density = veach::space::terrain_density(*system, home);

auto& universe = veach::services(world).get<veach::space::UniverseWorld>();
veach::Entity planet = universe.fixed_frame(home.index);       // the body-fixed frame entity
auto sun = universe.local_sun(home.index, position, universe.tick());   // local solar time, sun elevation

veach::space::FlightCommand cmd = veach::space::fly(governor, params, controls, state, top);
```

```sh
build/debug/apps/slice/veach-slice --script scripts/slice_s0.01.luau --window             # fly it yourself
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out veach-slice --script scripts/slice_s1.luau --rhi metal --headless \
  --frames 20000                                                                           # the demo, screenshots
```

## Acceptance criteria

- **slice.AC1** — The stage hashes (G0, S1, B1, atmosphere) of the slice's system are the committed values at
  s = 1, 0.1 and 0.01; radii, GMs, relief, atmosphere tops and orbital periods follow ADR 0049's rules between them;
  days don't scale.
- **slice.AC2** — The home planet is solid, rotates, is tilted (38.6°), passes the day-length check, has a temperate
  N2/O2 atmosphere from the atmosphere stage with the √s top, and a moon.
- **slice.AC3** — Local solar time is 12:00 at the subsolar point and advances with the spin and the orbit; the
  slice's readout reports it where the pilot is.
- **slice.AC4** — The governor and the handoff bands are the same in meters at both scales; a heading carried over
  the pole stays continuous; out-of-range settings are refused with the fix.
- **slice.AC5** — Through the engine on the null RHI, with gamepad events recorded by `--record`: from orbit through
  the cruise drive, the atmosphere-top handoff and the surface scene, a hover at 50 m and a landing near the site,
  at s = 0.01 and s = 1; `--replay` repeats it to the same state hash every frame; the landing hashes are committed.
- **slice.AC6** — In the same runs the pilot gets out, digs three times and stands more than 2 m below the undug
  ground; the final hashes are committed.
- **slice.AC7** — Screenshots of both scales (orbit, descent, near the ground, landed, the hole) and frame times at
  1440p on the M5 Max are in [docs/research/spikes/sp-15](../../research/spikes/sp-15-first-slice.md).

Tests: `[slice.AC1]` to `[slice.AC4]` in `tests/space_realization_tests.cpp`, `[slice.AC5] [slice.AC6]` in
`tests/slice_tests.cpp` (one case per scale), all in `veach-tests`.

## Out of scope

- Re-entering the craft, flying off again, other bodies than home and its moons drawn (they are frames on rails
  with no terrain), the moon as a landing target: follow-ups in the backlog.
- Entry drag and heating (ADR 0059): the assist absorbs drag; none is modeled yet.
- SOI handoffs between bodies (gravity answers per body frame), rails warp with physics (the clock runs at 1).
- universe.json (ADR 0055): the `Universe` component stands in for the genesis file.
- Measurements on M1 and M3 Pro hardware (SP-21, VCH-097).

## Open questions

None.

## Links

- ADRs: [0049](../../adr/0049-scale-model.md), [0059](../../adr/0059-gravity-flight-travel.md),
  [0080](../../adr/0080-amendments-from-sp-13.md), [0085](../../adr/0085-amendments-from-vch-111.md),
  [0088](../../adr/0088-the-realization-layer-and-the-first-slice.md)
- Backlog: VCH-115, VCH-091
- Findings: [SP-15](../../research/spikes/sp-15-first-slice.md)
- Guides: [the space toolbox](../../guides/space-realization.md)
