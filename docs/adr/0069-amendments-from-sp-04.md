# 0069: Amendments from SP-04: the phase formula, the Kepler starter, binary pairs, pausing the clock

Date: 2026-11-27

## Status

Accepted. Supersedes the phase formula of [0047](0047-universe-clock.md) and the Kepler solver and `binary_pair`
model of [0048](0048-bodies-on-rails.md); everything else in them stands. Validated by SP-04 (VCH-080,
[findings](../research/spikes/sp-04-orrery-clock.md)).

## Context

SP-04 built the universe clock and bodies on rails in `space/`: exact phase over 2^53 ticks, resonances that never
drift, a Sol preset within the accuracy of JPL's published Keplerian formula, the sun computed from the system with
identical noon elevations at s = 1 and s = 0.01 (53.40°, 30.00°, 6.60° at 60°N), canonical hashes equal in Debug and
Release. It found two formulas in the ADRs that are wrong as written, one model that composes badly, and one missing
clock rate:

- ADR 0047's phase, `((Δtick mod period_q) × 2^16) mod period_q` in int64, overflows when `period_q × 2^16 ≥ 2^63`,
  that is for any period over 1.13 years (Mars and every outer planet), not 74,000 years as the ADR says.
- ADR 0048's Kepler starter (`E₀ = π` for e > 0.8) needs 16 Newton steps at e = 0.9999, not 12; a unit test pins a
  1e-6 rad miss with 12.
- A `binary_pair` motion model cannot reuse other relative-motion models: the Earth-Moon pair needs the precessing
  secular model, not a plain Kepler orbit.
- Inspecting the orrery over MCP needed the clock paused, which ADR 0047's `rate ≥ 1` forbids.

## Decision

1. **Phase:** `r = Δtick mod period_q`, then `r` is multiplied by 2^16 modulo `period_q` with 16 steps of
   `r = 2r; if r ≥ period_q: r −= period_q` on uint64 (exact, portable, 6.9 ns), and `phase = double(r) /
   double(period_q)`. The bound is `period_q < 2^63`: periods up to about 74,000 years.
2. **Kepler:** Newton on E with **6 iterations for every eccentricity**, starting from `E₀ = M + e·sin M` for e ≤ 0.8 and
   the cubic starter (the root of E³/6 ≈ M as e → 1, blended with M) above. Hyperbolic orbits keep a fixed count.
3. **Binary pairs are a barycentric split on the node**, not a motion model: a node carries a mass fraction, and its
   two children take opposite ends of whatever relative-motion model the node uses. One solve per pair is a caching
   follow-up.
4. **The clock may pause:** `rate = 0` is allowed for tools, MCP and single player; multiplayer rules of 0047 stand.

## Consequences

- Every period the generator produces gets exact phase; the orbit hashes of SP-04 are computed with these rules and
  checked on the macOS gate and Windows CI.
- The solver has one iteration count, which keeps its cost fixed: about 200 ns per body per evaluation on the M5 Max
  with dmath, almost all in its 14 sin/cos calls; reductions (precomputed spin trig, one solve per pair) are listed
  in the findings.
- Systems compose: a binary star, a circumbinary planet, a moon's moon and comets up to e = 0.9999 all evaluate through
  the same tree.

## Alternatives considered

- **A 128-bit product for the phase:** exact too, but MSVC has no portable `__int128`; the loop is portable and cheap.
- **More iterations with the π starter:** 16 everywhere costs more than a better starter.
- **Keeping `binary_pair` as a model:** every relative-motion model would need a binary variant.
