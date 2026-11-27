# 0048: Bodies on rails: a barycenter tree of registered motion models; suns and eclipses as queries

Date: 2026-11-27

## Status

Accepted. Amends [ADR 0038](0038-physical-sky.md): the sun comes from the star system, and `Sky` takes N sun
directions instead of computing one from Earth astronomy.

## Context

Bodies must move, spin and tilt so that days, years, seasons, local time and eclipses follow, at every scale
([0049](0049-scale-model.md)), deterministically ([0043](0043-determinism-tiers-and-strict-floating-point.md)) and at
no sync cost ([0056](0056-server-authoritative-multiplayer.md)). Today `sun_direction(const Sky&)`
(`engine/render/src/atmosphere.cpp`) is Earth astronomy in float32: one sun, one planet, +Y as zenith.

Integrating bodies (n-body) is chaotic: one ulp of difference grows exponentially, so clients must agree bitwise
forever and every save holds every body's state. Closed-form Keplerian motion is a pure function of time: nothing to
sync or save, any tick queryable in O(1), unlimited warp, ~71-77 ns per body measured on the M5 Max. It loses
perturbations, which players of this genre don't see; the generator must space orbits because nothing corrects them.
Research: [02-celestial-mechanics-time](../research/02-celestial-mechanics-time.md).

## Decision

**Bodies are on rails.** Nothing about a body's motion is stored, sent or integrated; it is regenerated from seed,
manifest and tick ([0047](0047-universe-clock.md)). Celestial evaluation is canonical tier.

**The orbit tree** (`space/`): nodes are entities with `OrbitNode { parent; model; elements; period_q; gm_game; }`.

- `model` is a **registered motion model** by stable string id, not an enum: `space:kepler_elliptic`,
  `space:kepler_hyperbolic`, `space:binary_pair` (two children around their barycenter, one Kepler solve split by mass
  ratio), `space:fixed` (roots, rogue bodies), `space:scripted` (a pure Luau `f(tick) → pose` in the deterministic
  sandbox, [0052](0052-procgen-runtime.md)). C++ modules register more as `MotionModel`s with fixed-cost evaluation.
- `elements`: a, e, i, Ω, ω and M₀ at the epoch, physical, realized by the scale model.
- `period_q` (int64, 2^-16 tick units) is **stored, not recomputed** from a and GM. The generator sets it from
  `gm_game` by default; it is the seam that lets a creator decouple a year from its orbit radius. Moon and low orbits
  stay GM-consistent because ships share them.
- Planets are S-type children of a star or P-type children of a `binary_pair`; moons of moons nest.

**Evaluation** per fixed step of the active system's subtree (tens to hundreds of nodes), parent before child, in
double, each node in its parent's frame; results are the frame poses of
[0045](0045-large-world-positions-and-reference-frames.md). Kepler: Newton on E with a **fixed iteration count** (6 for
e < 0.7, 12 up to e = 0.9999), starter E₀ = M + e·sin M (π for e > 0.8); hyperbolic by Newton on sinh, fixed count.
Position from E directly (`a(cos E − e)`, `b sin E`), velocity analytically. Trig through `veach::dmath`; no `atan2`
or `asin` in the authoritative path.

**Spin.** `Spin { period_sid_q; phase0; pole; }`, the pole as obliquity and its longitude. Orientation = pole
rotation × R_z(2π × phase(tick)). Creators specify the **solar day**; the sidereal period follows from
1/P_sol = 1/P_sid − 1/P_orb. P_sid = P_orb gives tidal locking. No precession.

**Queries** (C++, mirrored in `@veach` and MCP), pure in (body, latitude, longitude, tick): `sun_direction_local`,
`sun_elevation`, `local_solar_time`, `day_of_year`, `year_number`, `subsolar_latitude`, `season_phase`,
`illuminance_top` (from **physical** distance and luminosity, never scaled), `eclipse_fraction`. Calendars are Luau
formatters over (body, tick).

**Eclipses and ring shadows.** `occluders(observer_body, observer_pos, star, tick)` returns the bodies that can cover
the star (ancestors, siblings and their children), each with angular radius asin(R/d) and separation.
`eclipse_fraction` is one minus the circle-overlap area over the star's disk. A ring shadow intersects the ray to the
star with the ring plane and reads a 1D radial opacity profile. Gameplay uses (solar power) are canonical; the renderer
consumes the same list ([0063](0063-astronomical-rendering.md)).

**`Sky` changes shape (engine, `engine/render`).** It takes up to N suns, each `{ direction in the camera's local
frame, illuminance, angular_radius, color }`, plus up and altitude from the view's body context. The toolbox writes
them from the queries. The Earth formula stays as a convenience for non-space projects, used only when no suns are
supplied.

## Consequences

- Seasons, eccentric orbits, retrograde spin, tidal locking, moons and binary suns need no special cases.
- Saves and packets carry no body state; MCP answers "where is this moon at tick t" for any t.
- No emergent orbital dynamics; the generator must guarantee Hill spacing and stability.
- Each extra sun costs the renderer; how many per quality tier is [0064](0064-quality-presets.md)'s call.

**Validated by** VCH-080 (orrery over MCP: a "Sol" preset within JPL's approximate positions; orbit hash equal on Mac
and Windows; noon sun elevations of 53.4°, 6.6° and 30.0° at latitude 60° on a 23.4°-tilted planet, at s = 1 and 0.01)
and VCH-086 (two suns and an eclipse lighting a per-body atmosphere). **Reopen if** the product wants emergent orbital
dynamics as gameplay (a server-authoritative n-body module with state sync), or correctly rounded sin/cos cost far more
than libm (a fixed polynomial, still canonical).

## Alternatives considered

- **Full n-body** (Universe Sandbox, Principia): chaotic, synced forever, accuracy-capped under warp.
- **Selective simulation** (Outer Wilds): hand-tuned for one authored system.
- **Patched conics or restricted n-body:** right for ships, on top of this ([0059](0059-gravity-flight-travel.md)).
- **Keep Earth astronomy in `Sky`:** correct for one sun, one planet and a circular year only.
