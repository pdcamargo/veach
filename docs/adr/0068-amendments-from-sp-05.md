# 0068: Amendments from SP-05: orbit scale at least body scale, radius in the system stage, habitability bias

Date: 2026-11-27

## Status

Accepted. Amends [0049](0049-scale-model.md) and [0051](0051-staged-generation-pipeline.md), and sets a default left
open by [0066](0066-owner-decisions-research-phase.md). Validated by SP-05 (VCH-081,
[findings](../research/spikes/sp-05-system-statistics.md)).

## Context

SP-05 generated 100,000 systems headless through stages S1 (system) and B1 (body) with zero stability, Roche and Hill
violations, the same hash on 1, 6 and 18 threads, and equal records at s = 1, 0.1 and 0.01. It found three things the
ADRs got wrong or left open:

- ADR 0049 says that with separate body and orbit factors "the generator's Hill-spacing check absorbs" the change in
  sphere-of-influence ratio. That is true only when `s_p ≤ s_o`. With `s_p = 0.1` and `s_o = 0.01`, 11 % of moons end
  up outside their planet's Hill radius after realization: the planet shrinks less than the orbit it sits on.
- ADR 0051 computes a planet's radius in its body stage (B1), but the system stage (S1) places moons and rings against
  the planet's Roche limit, which needs the radius.
- The owner chose "habitable planets common" (ADR 0066). Physics alone already gives 45 % of systems a potentially
  habitable body; a bias of 0.5 gives 69 %.

## Decision

1. **`s_o ≥ s_p` always.** The advanced setting that separates orbit scale from body scale accepts only orbit factors
   at least as large as the body factor; anything else is refused at universe creation with an error that names the
   rule. The default `s_o = s_p` is unaffected.
2. **A body's radius is computed in S1**, with its mass, from composition and the mass-radius relation; B1 derives
   everything else (density, gravity, escape velocity, temperature, atmosphere retention) from S1's records. Stage
   versions record the move.
3. **Habitability bias defaults to 0.25**, implementing the owner's "common" without making most systems habitable;
   it stays a creator setting.

## Consequences

- Realized moons stay inside their Hill radius at every allowed scale combination; SP-05's check runs on realized
  geometry, not only on records.
- Moon and ring placement no longer depend on a later stage, so S1 stays self-contained, as ADR 0051 requires.
- Creators who want even more habitable worlds raise the bias.

## Alternatives considered

- **Clamp moon orbits in the realization layer when `s_p > s_o`:** keeps the setting free, but changes moon periods
  and eclipses behind the creator's back.
- **Bias 0.5:** 69 % of systems habitable, more than "common" needs.
