# 0080: Amendments from SP-13: fictitious forces in rotating free flight, velocity-tracking scenes, handoff bands

Date: 2026-11-27

## Status

Accepted. Amends [0058](0058-physics-jolt.md) ("no Coriolis or centrifugal forces", co-moving scenes) and
[0059](0059-gravity-flight-travel.md) (handoff altitudes, as the toolbox policy [0045](0045-large-world-positions-and-reference-frames.md)
leaves to it). Validated by SP-13 (VCH-089, [findings](../research/spikes/sp-13-walk-hover-land.md)).

## Context

SP-13 walked a character, hovered and landed a craft and left crates on a rotating planet at s = 0.01 and 1, with
inertial and body-fixed frames, a surface physics scene and a free-flight scene. Over 24 simulated hours (3,201
surface-scene re-origins) a static base drifts 8.5e-14 m at s = 1 and 0 at s = 0.01; standing characters and sleeping
crates under 6e-14 m. The largest |Δv| at a handoff is 2.8e-6 m/s (free flight into the surface scene, at 95 m/s);
inertial ↔ body-fixed is 2.7e-13 m/s, and scene re-origins are exactly 0. A recorded landing replays through
`--record`/`--replay` to the same hash at both scales. The whole scene steps in about 0.013 ms. Two rules and one gap
in the ADRs showed up.

## Decision

1. **Fictitious forces apply in rotating-frame free-flight scenes.** Bodies flying freely in a body-fixed frame get
   Coriolis (−2ω × v) and centrifugal (−ω × (ω × r)) accelerations each step, computed in double. Without them an
   unpowered fall through the handoff drifts from an inertial control by 3.1 m/s and 49 m over 90 s at s = 1 (with
   them: 0.67 mm/s, 1.6 cm). The **surface scene keeps 0058's rule**: no fictitious forces; its bodies rest on the
   ground or move slowly, and 0049's day-length validation bounds the error.
2. **A free-flight scene tracks its body's velocity as well as its position.** Jolt stores velocities in float32 even
   with double positions, so the scene's frame moves with the tracked body: its origin advances in double on exact
   tick time, and Jolt sees only the residual velocity. Without it, rounding reaches 0.98 mm/s at the 20 km/s governor
   cap.
3. **Handoff bands (toolbox policy):** an entity is body-fixed below the atmosphere top (100 km × √s_p for an
   Earth-like body, the realized `s_a` thickness in general) and inertial above 1.1 × that height; a craft joins the
   surface physics scene below 500 m above ground and leaves it above 750 m. The gaps are hysteresis.

## Consequences

- Flight through the atmosphere-top handoff matches the inertial trajectory to millimeters, at both scales.
- Bodies entering the surface scene faster than its collider region can serve fall through: SP-13 lost one at
  425 m/s. Fast bodies there need continuous collision detection and the swept collider region of
  [0077](0077-amendments-from-sp-12.md); the first slice caps the entry speed until then.
- The walking character lost ground contact on 13 or 14 of 5.18 M steps a day (never more than 1 cm), probably at
  chunk seams; to be investigated with `engine/physics`.

## Alternatives considered

- **Keep free flight inertial all the way down to the surface scene:** avoids fictitious forces, but then the
  atmosphere, wind and the governor all work in a frame that sweeps under the craft; not measured.
- **Double velocities by patching Jolt:** a fork to carry for what a moving origin gives exactly.
