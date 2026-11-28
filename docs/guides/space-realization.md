# The space toolbox: a universe from a seed, and the first slice

`space/` (`veach::space`) turns a universe seed and the creator's settings into a star system to fly in
([spec](../specs/first-slice/spec.md), ADRs [0049](../adr/0049-scale-model.md), [0059](../adr/0059-gravity-flight-travel.md),
[0080](../adr/0080-amendments-from-sp-13.md), [0088](../adr/0088-the-realization-layer-and-the-first-slice.md)).
`apps/slice` (`veach-slice`) is the game built on it: from orbit to a dug hole.

## Run the slice

```sh
build/debug/apps/slice/veach-slice --script scripts/slice_s0.01.luau --window     # fly it yourself (s = 0.01)
build/debug/apps/slice/veach-slice --window                                      # scripts/slice.luau: s = 0.1
VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out build/release/apps/slice/veach-slice \
  --script scripts/slice_s1.luau --rhi metal --headless --frames 20000          # the demo: screenshots, frame times
```

Controls (gamepad / keys): right stick Y or R/F lift, left stick or WASD strafe and walk, right stick X or Q/E yaw,
right trigger or Space engine (on above half), south button or X get out, west button or G dig. With the engine on,
sticks centered means "hold this place": the assist targets zero surface-relative velocity. Above the atmosphere top
the cruise drive flies the sticks' velocity directly; below it the thrusters chase it within 40 m/s².

The readout is logged every 5 s and on every change of mode (`slice:` lines) and published as `SliceReadout` on the
`slice` entity (`get_component` over MCP): mode, frame, scene, altitude, height above the ground, speed, the
governor's cap, local solar time and the sun's elevation where the craft or pilot is, latitude, longitude, scale,
the canonical state hash.

## The scale is configuration

The three scripts differ only in `scale`:

```lua
veach.spawn("universe"):set(c.Universe, { seed = "42", system = "S:1000400100008000", scale = 0.01, start_tick = 0 })
```

`Universe` holds the genesis settings ([ADR 0055](../adr/0055-universe-identity-and-versioning.md)): `seed` and
`system` as strings (64-bit, [ADR 0046](../adr/0046-64-bit-values-across-luau-json-mcp.md)), `scale` (s_p, 0.01 to
1), `orbit_scale` (s_o, 0 = s_p), `atmosphere_exponent` (s_a = s_p^e, [ADR 0081](../adr/0081-creator-control-over-atmosphere-scaling.md)),
`relief_exponent`, `gravity` and `day` multipliers, `habitability_bias`, `home` (the body with terrain; empty: the
first potentially habitable planet) and `start_tick`. Set it once: a second set with other values is refused.

## Realization in C++

```cpp
#include <veach/space/realization.hpp>
auto system = veach::space::realize({.seed = 42, .system = address, .scale = 0.01});  // Result<StarSystem>
const veach::space::RealizedBody& home = *system->home();
system->hashes;          // the stages' record hashes: the same at every scale
system->orbits;          // space::System: the bodies on rails, gameplay GMs (g R²)
home.atmosphere;         // the render record from the composition, at (s_p, s_a)
auto density = veach::space::terrain_density(*system, home);   // a voxel::ProgramDensity
```

- Stages: G0 `space:universe`, S1 `space:system`, B1 `space:body.physics` and `space:body.atmosphere` (space/gen),
  memoized by `veach::procgen::StageRuntime`. None takes a scale.
- Scale, applied after: radii and moon orbits × s_p; planet orbits and the star × s_o; atmosphere heights × s_a with
  coefficients × 1/s_a; relief × s_p^relief_exponent; GM = g R²; periods from those GMs; sidereal days physical.
- `gravity(body, r)` is the body's pull at a body-centered point; `surface_radius(density, n)` the ground along a
  direction (canonical bisection).

## In a world: UniverseModule

```cpp
engine.import<veach::PhysicsModule>();
engine.import<veach::TerrainModule>();
engine.import<veach::space::UniverseModule>();
auto& u = veach::services(world).get<veach::space::UniverseWorld>();
u.fixed_frame(home.index);     // universe::system::b1::fixed, the body-fixed frame
u.orbit_frame(home.index);     // universe::system::b1, its inertial center frame
u.local_sun(home.index, body_fixed_position, u.tick());
```

Every body frame is written in PostSimulation for the end of the step from the tick (ADR 0085): positions and
velocities from the orbit tree, orientations from the exact spin phase. The gravity hook answers every query in a
body's two frames with that body's pull. Home and its moons get a terrain body; the renderer gets the nearest
atmosphere, the sun at its physical illuminance and the system's space backdrop.

## The sky beyond the bodies

`space::system_backdrop(config)` (`veach/space/backdrop.hpp`, [ADR 0090](../adr/0090-space-backdrop.md)) turns the
universe seed and the system address into the engine's `render::Backdrop`: stars, the galactic band, nebulae.
Canonical and independent of the scale: every system has its own sky, the same on every revisit and at s = 0.01 and
s = 1. Where the system is in the galaxy shapes it (`space::galaxy_view`, a stand-in disk and bulge until the galaxy
query exists): toward the center the band is brighter, the bulge bigger and the anticenter as bright as the center;
far above the plane the band is fainter, wider and lower, with fewer nebulae. The galactic plane is turned at random
against the system's. UniverseModule turns it into render space every frame (it stays fixed while the planet spins
under a body-fixed camera).

How bright it looks is the creator's: a `SpaceBackdrop` component anywhere in the world
(`exposure_compensation`, `exposure_follow`, `visible`; [Luau API](luau-api.md)). The defaults keep it visible next
to a sunlit planet, about 1.7 stops dimmer than in deep space, and never let it move auto-exposure. Screenshots:
[space-backdrop](../research/spikes/space-backdrop/README.md).

## Flight policy

`veach/space/flight.hpp`, all in meters and seconds and the same at every scale:

| | |
|---|---|
| Governor | `max(100 m/s, height / 5 s)`; cruise `max(100 m/s, height / 7 s)` |
| Cruise limit | `min(cruise cap, √(2 · 20 m/s² · top) + (altitude − top) / 7 s)`: the thrusters' entry speed at the top |
| Frames | body-fixed below the atmosphere top, inertial above 1.1 × it |
| Scenes | surface scene below 500 m above the ground, free flight above 750 m; entry at most 150 m/s |
| Assist | target = sticks × cap in the heading frame; `a = (target − v) / 0.5 s − g`, at most 40 m/s² |
| Heading | up follows −ĝ (0.15 s), forward transported by projection, never from north |

## Tests

`[slice.AC1]`–`[slice.AC4]` (`tests/space_realization_tests.cpp`) check the stage hashes across scales, the scale
rules, the home planet, local time, the bands and the heading. `[slice.AC5] [slice.AC6]` (`tests/slice_tests.cpp`)
fly the whole slice through the engine on the null RHI at s = 0.01 and s = 1, recorded and replayed, with committed
landing and dig hashes. Findings and screenshots: [SP-15](../research/spikes/sp-15-first-slice.md).
