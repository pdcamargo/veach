# 0057: Simulation LOD: four tiers per observer, baseline plus deltas plus clock functions, verified catch-up

Date: 2026-11-27

## Status

Accepted.

## Context

Only a few square kilometers around each player are ever in view, yet herds, traffic, factions, bases and eruptions
should go on while nobody watches. Simulation is fixed-step ([0018](0018-engine-lifecycle-and-phases.md)),
single-threaded until VCH-015, and replays bit-exactly. Population grows with the square of body scale
([0049](0049-scale-model.md)); cost must not. And a setting that changes how far full simulation reaches must not change
what happens, or two players' hardware gives two universes. Research:
[07-simulation-lod](../research/07-simulation-lod.md).

## Decision

**State contract.** World state = generated baseline ([0051](0051-staged-generation-pipeline.md)) + persisted deltas
([0055](0055-universe-identity-and-versioning.md)) + functions of the universe tick ([0047](0047-universe-clock.md)).
A record with no delta is the baseline and costs nothing to store.

**Four tiers per observer**, chosen per sim cell or agent from the distance to the nearest observer, with hysteresis
(a larger radius out than in):

| Tier | What exists | Rate |
|---|---|---|
| T0 Live | Individuals with physics, animation and full AI | Every fixed step |
| T1 Reduced | Individuals without physics or animation, coarse movement on schedules | Every Nth step, staggered by id |
| T2 Aggregate | Numbers per sim cell: density per species, stock per station, influence per faction | One coarse tick per domain period, aligned to the clock |
| T3 Dormant | Nothing runs: a persisted delta with its `last_tick`, or the baseline | Never |

T0 and T1 radii are in meters and never follow scale; they are quality-preset values
([0064](0064-quality-presets.md)). **Each body has a fixed sim-cell grid**: the cube-sphere quadtree of
[0050](0050-universe-addresses-and-seeds.md)'s surface keys at level 5, 6,144 cells per body at every scale (cell area
follows the body's radius squared). Cell ids are save keys, so changing the level is a save migration. Promotion from
T3 to T2 happens ahead of the observer by approach speed times a lead time ([0059](0059-gravity-flight-travel.md)), so
catch-up runs at promotion, not at landing.

**Catch-up.** Every domain implements `advance(state, from_tick, to_tick)` with the first technique that fits, in this
order: (1) clock function (pure function of seed and tick: weather, eruption schedules, voyages, herd routines);
(2) closed form (logistic recovery, regrowth timers); (3) event jumps (piecewise-constant rates, such as a base's
production graph until a buffer fills); (4) capped coarse steps (the same T2 step function, N times, with a cap);
(5) snap to baseline (when elapsed time exceeds several relaxation times, the delta is deleted). Each domain declares
its technique.

**Verified equivalence.** Running a domain at T2 for N ticks and running it dormant then calling `advance()` must give
the same state hash. Every domain has this test in CI, headless on the null RHI.

**Determinism rules.**

- Coarse ticks fire when `tick % period == hash(id) % period`. They never read `Time.elapsed`, Flecs `.interval()` or
  Flecs `.rate()`, which drift or misalign after a load.
- Randomness is the counter-based hash of [0044](0044-counter-based-hash-randomness.md), keyed by
  (universe seed, domain, id, tick, draw). `veach::Random` is never used here.
- Authoritative aggregate state is integer or fixed point. Transcendentals go through `veach::dmath`
  ([0043](0043-determinism-tiers-and-strict-floating-point.md)) or a table.
- A coarse step reads the previous tick's state and writes only its own record. Cross-record effects are messages
  sorted by (target, source, sequence) and applied at the next tick, so a tick is an order-independent parallel-for
  once VCH-015 lands.
- Tier changes happen at tick boundaries. What a materialized cell contains is a function of the aggregate, the tick
  and the seeds; only which cells are materialized depends on observers.

**Materialization and fold-back.** Ambient individual j of species s in cell c has the seed
hash(universe, "fauna", c, s, j); the first n slots by hash rank are live, so the same animals remain when density
drops. When individuals dematerialize, **only player-caused, declared perturbations** (killed, tamed, harvested,
destroyed...) change the aggregate; everything else is discarded. T0 is a presentation of T2, so its radius can be a
quality setting.

**Coupled domains** (factions, economy) tick only inside an active set (unrelaxed systems, systems within k jumps of an
observer, a creator-defined core); outside it they freeze. This layer depends on where players went; the docs say so.

**Seams and placement.** A domain is a module implementing `SimDomain` (records, step, `advance()`, materializer,
fold-back). The catch-up loop stays in C++ and calls Luau steps with a step cap, because Luau has no interrupts yet.
The hash and the stagger helper are engine code (`engine/core`); tiers, the delta store, `SimDomain` and the domains
are `space/` ([0065](0065-engine-areas-for-the-toolbox.md)). MCP gains `sim_tiers` and, headless only,
`advance_clock {ticks}`.

## Consequences

- Hardware decides how many creatures you see, never the herd's numbers, the war or a base's inventory.
- Saves stay small: relaxation deletes deltas.
- Every domain costs a catch-up implementation and an equivalence test.
- RNG keys, aggregate formats, coarse periods and the cell grid are versioned under 0055.
- **Validation:** VCH-093 (SP-17): live and caught-up hashes equal, and a year of 6,144 sim cells caught up in under
  1 ms on the M5 Max. It does not block the first slice.
- **Reopen if** capped coarse stepping cannot match live T2 bit for bit in practice (keep the active set always
  ticking instead), catch-up measures far slower than the 1 ms bound, or the owner wants emergent individual ecology
  (individuals as presentation is then the wrong rule).

## Alternatives considered

- **Simulate everything everywhere.** Cost grows with the universe and with scale squared.
- **Freeze everything away from players.** The universe stops while you are away.
- **Fold all live results back.** Outcomes would depend on the T0 radius, so on hardware.
- **Lotka-Volterra populations.** Neutrally stable, no closed form; logistic recovery is the default.
