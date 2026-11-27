# 0049: The scale model: body, orbit and atmosphere scale factors; what never scales; derived quantities

Date: 2026-11-27

## Status

Accepted (the mechanism). The default values are product decisions pending the owner's review packet; this ADR
records the working defaults.
Defaults confirmed by the owner in [0066](0066-owner-decisions-research-phase.md).

## Context

A creator picks how big the universe is, from real scale (Earth radius 6,371 km) to NMS-like compression (63.7 km at
s = 0.01). The same seed must give the same planet (composition, climate, biomes, life) at every scale; only geometry
changes. The research questions disagreed on details: whether stars follow the planet factor (they must not, for
eclipses to stay coherent), how gravity follows radius, how thick atmospheres are at small s, and where s may enter
generation. The dev lead's resolution is conflict 5 in [_decisions](../research/notes/_decisions.md). Research:
[02-celestial-mechanics-time](../research/02-celestial-mechanics-time.md) §6,
[03-generation-pipeline](../research/03-generation-pipeline.md) §7.5, [05-procgen-graph](../research/05-procgen-graph.md)
§5, [10-gravity-flight-travel](../research/10-gravity-flight-travel.md).

## Decision

**Three factors**, stored in the universe genesis ([0055](0055-universe-identity-and-versioning.md)):

| Factor | Scales | Default rule |
|---|---|---|
| `s_p` (body) | planet, moon and asteroid radii; moon orbits around their planet; planetary-band terrain (continents, ranges, basins); rivers from scaled catchments, with a width floor in meters | the creator's choice |
| `s_o` (orbit) | planet orbits around stars, star radii, binary separations | `s_o = s_p`; a separate value is an advanced setting |
| `s_a` (atmosphere) | atmosphere thickness and scale heights; scattering and absorption coefficients × 1/s_a, keeping optical depth and sky color | `s_a = sqrt(s_p)` (10 km of atmosphere at s = 0.01) |

Moons follow `s_p` and stars `s_o`, so angular sizes and eclipses stay coherent even with separate factors.

**Never scaled (meters, seconds, physical):** players, creatures, props, buildings, ships, caves (cave band depth in
meters), edit brushes, the LOD0 cell ([0053](0053-terrain-density-field.md)), detail-band terrain, human-scale haze
and fog, object density per m², and every physical record value (temperature, pressure, composition, luminosity).
Illuminance uses physical distance. Interstellar distances are outside the model: the galaxy has its own size
([0050](0050-universe-addresses-and-seeds.md)).

**Derived quantities** (computed by the realization layer, never stored):

- **Gravity.** Surface g is the physical value times a creator multiplier. Gameplay `GM = g·R²` per body, so GM scales
  with the square of the body's radius factor: `s_p²` for planets and moons, `s_o²` for stars. With `s_p ≠ s_o` the
  sphere-of-influence ratio moves by (s_p/s_o)^0.2, which the generator's Hill-spacing check absorbs.
- **Orbits.** Radius ∝ s and GM ∝ s², so orbital speeds and periods scale with √s. Low orbit around an Earth-like
  planet: 7.8 km/s and 92 min at s = 1, 2.5 km/s and 29 min at 0.1, 0.78 km/s and 9 min at 0.01. The month follows
  √s_p (27.3, 8.6, 2.7 days), the year √s_o (365, 115, 36.5 days).
- **Day length.** Physical sidereal period times a creator multiplier, independent of R. Validated per body: **warn**
  when equatorial rotation speed exceeds 30 % of the surface orbital speed √(gR), **refuse** above 70 %.
- **Relief.** Planetary-band relief scales with `s_p^relief_exponent`, a creator setting; detail-band relief never
  scales. At 0.5 an 8.8 km mountain is 880 m at s = 0.01 (88 m with plain s_p).
- **Travel.** The governor caps speed at `max(v_floor, d/τ)` in meters, identical at every scale
  ([0059](0059-gravity-flight-travel.md)): closing a distance ratio takes τ·ln(ratio) at every s (a moon-like hop is
  48 s at s = 0.01, 0.1 and 1), and the chunk streaming load per second is scale-independent. Starting values
  τ = 5 s near bodies, 7 s in cruise, v_floor = 100 m/s, replaced by VCH-085's measured chunk budgets.

**No stage ever reads s.** Generation stages G0 to B5 produce physical records
([0051](0051-staged-generation-pipeline.md)); the stage context has no scale field. Scale is applied when records
become geometry. CI asserts stage hashes equal at s ∈ {1, 0.1, 0.01}.

**s is immutable once anchored.** Placed objects and edit positions depend on the realized radius, so they differ
across scales even though records don't. The factors may change only while no body is anchored; after that, a change
is a new universe ([0055](0055-universe-identity-and-versioning.md)).

**Where it lives:** the scale model is toolbox (`space/`, the realization layer). No engine code reads a scale factor
([0042](0042-space-toolbox-layering.md)).

**Defaults: see the review packet; until then** `s_p = 0.1` (an Earth-like planet ≈ 637 km radius), supported range
0.01 to 1, `s_o = s_p`, `s_a = sqrt(s_p)`, surface gravity ×1, day length ×1 (validated), year following √s_o,
relief exponent 0.5, human-scale haze on.

## Consequences

- One code path serves every scale; the first slice runs at s = 0.01 and 1 by configuration.
- Surface gravity feels physical everywhere; orbital periods are not real at compressed scale, by design.
- Two universes at different scales share continents and climates but not individual trees.
- Every creator's tuning of flight, fuel and travel depends on the gravity rule; changing it later is expensive.

**Validated by** VCH-081 (stage hashes equal across s), VCH-085 (descent at s = 0.01 and 1: peak chunks/s within 20 %
across scales; the relief exponent judged by eye from its screenshots), VCH-086 (atmosphere ground to orbit at
s_a = sqrt(s)), VCH-089 (walk and land at both scales) and VCH-091 (the first slice at both scales). **Reopen if**
VCH-085's chunk rate differs by more than 20 % across scales, VCH-086's √s atmosphere looks wrong from orbit or ground,
or the owner wants real-length years at compressed scale (the stored `period_q` of [0048](0048-bodies-on-rails.md)
allows it for planet orbits only).

## Alternatives considered

- **Generate at the scaled size:** escape velocity drops by √s, atmospheres are stripped, the planet changes.
- **Keep the star's physical GM while orbits shrink:** 8.8-hour years at s = 0.01, the Moon outside Earth's sphere of
  influence.
- **Atmosphere scaled by s_p:** a 1 km atmosphere at s = 0.01, thinner than the clouds players fly through.
