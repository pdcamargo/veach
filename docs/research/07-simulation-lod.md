# 07: Simulation LOD and off-screen catch-up

How fauna, flora, ecosystems, NPCs, factions, economy, traffic, automated bases, weather and eruptions are simulated
near players, far from them and nowhere near them, and how a place catches up when someone comes back, while staying
deterministic. Respects the (a) items for Q7 in [00-scope.md](00-scope.md) (section 9). Raw notes and every source:
[notes/07-simulation-lod-notes.md](notes/07-simulation-lod-notes.md).

**Short answer.** The world's state is the generated baseline, plus the deviations we persist, plus things that are
functions of the universal clock. Nothing is simulated unless someone is close enough to see it. Far away, the world
is a set of numbers that catch up when someone comes back, preferably in closed form. Full simulation near a player
only shows those numbers; it never decides an outcome on its own, except for what the player causes.

## 1. The problem in Veach's terms

A seed-generated universe has billions of bodies. Only a few square kilometers around each player are ever in view.
The brief wants the universe to feel alive anyway: creatures in herds, ships trading between stations, factions
gaining and losing territory, bases producing while the owner is away, storms crossing a continent, a volcano that
erupted while you were in another system and left a lava field. Veach's constraints make this a specific problem:

- **Fixed 60 Hz, single-threaded, deterministic.** Simulation runs in fixed 1/60 s steps
  ([ADR 0018](../adr/0018-engine-lifecycle-and-phases.md)). There are no worker threads until the job system lands
  (VCH-015). Whatever runs per step comes out of one core's budget.
- **One RNG stream.** `veach::Random` is a single PCG32 singleton. Every draw depends on how many draws came before
  it, so adding a creature reshuffles every later random number, and a value computed in catch-up can never equal
  the same value computed live. Simulation LOD needs randomness keyed by *what* and *when*, not by call order.
- **Record/replay and MCP.** A run must replay bit-exactly from seed and inputs
  ([spec](../specs/record-replay/spec.md)). Agents inspect the world over MCP. An off-screen simulation that agents
  can't query and replay can't be tested.
- **Luau extensibility.** Creators extend behavior with strict Luau. Script systems get batched arrays
  ([ADR 0007](../adr/0007-scripting-luau.md)) but have no yielding or interrupts, so an unbounded catch-up loop in
  Luau would hang the engine.
- **Scale is a setting.** At s = 1 a planet has ~100× the surface area it has at s = 0.1. Creature density per m²
  must stay fixed (00-scope), so total population grows as s_p², and the simulation cost must not grow with it.
- **Multiplayer later (Q6).** Two players in different systems at once, and a server that owns the clock. Anything
  that is a pure function of seed and clock needs no syncing; everything else does.
- **The hardware range.** A quality setting that changes how far the full simulation reaches must not change what
  happens in the world. Otherwise an M1 player and an M3 Pro player see different universes, and multiplayer desyncs.

The question is not "how do we simulate a planet". It is **what state exists at all**, who may change it, and how
it advances when nobody is looking.

## 2. Options, with precedents

### 2.1 The overall model

| | Model | Precedent | What it means for Veach |
|---|---|---|---|
| **A** | **Freeze when unobserved.** Unloaded places stop; on return they are exactly as left. | Star Citizen freezes unobserved containers ([OCS](https://starcitizen.tools/Object_Container_Streaming)); Space Engineers simulates physics only near players ([Rosa 2020](https://blog.marekrosa.org/2020/08/space-engineers-server-optimizations/)) and stops far grids ([wiki](https://spaceengineerswiki.com/Trash_Removal)); Kenshi deletes wilderness squads that leave the loaded area and stores town squads (player reports, [Steam](https://steamcommunity.com/app/233860/discussions/0/3276942988970004699)) | Simple and deterministic, but bases don't produce, nothing erupts, the universe is a museum. Fails the brief. |
| **B** | **Simulate everything at full rate.** | Factorio: every active entity every tick, memory-bound on large bases ([FFF #204](https://factorio.com/blog/post/fff-204)); Eco: every plant and animal on a 24-hour server ([PC Gamer](https://pcgamer.com/eco-is-an-open-world-survival-game-that-will-let-you-destroy-the-planet)) | Works for one small world. Impossible at universe scale on one M1 core. |
| **C** | **Keep everything loaded running cheaply.** Full rules in a bubble, simplified rules outside, a periodic aggregate tick for the rest. | X4 high/low attention: everything is simulated, cheaply outside the bubble; low-attention combat skips line of fire, so outcomes differ by mode ([developer and players](https://steamcommunity.com/app/392160/discussions/0/6664812048260991709)); Elite's background simulation: faction influence per system on a ~daily aggregate tick ([forum](https://forums.frontier.co.uk/threads/when-and-what-is-the-tick.400292/)); Dwarf Fortress world activation with abstract armies ([DF wiki](https://dwarffortresswiki.org/Army)); Oblivion's Low Level Processing (modders, [Nexus](https://forums.nexusmods.com/topic/277365-ai-wont-leave-house)) | Proven for a bounded, authored region (X4's map, Elite's inhabited bubble). Its cost grows with the size of the simulated set, so it needs a bound. Two rule sets disagree (X4). |
| **D** | **Lazy, clock-derived world.** State = generated baseline + persisted deviations + functions of the clock. Places that nobody observes don't run; they catch up when observed. C runs only inside a bounded active set for systems that are coupled (factions, economy). | Pieces exist separately: on-rails orbits (Q2), idle-game offline progress ([Math of Idle Games](https://gamedeveloper.com/design/the-math-of-idle-games-part-i)), NMS creatures regenerated from seed (Origins reset fauna and kept terrain, [nomanssky.com](https://nomanssky.com/origins-update/)), AC Unity and Hitman NPC recycling ([GDC 2015](https://www.gdcvault.com/play/1022141/Massive-Crowd-on-Assassin-s), [GDC 2012](https://gdcvault.com/play/1015315/Crowds-in-Hitman)) | Cost grows with what's observed and what's been touched, not with universe size. Deterministic by construction. Demands discipline: every domain must say how it catches up. **Recommended.** |

No precedent I found combines all of D. Its parts are each shipped somewhere, and the academic framing ("simulation
LOD": lower the simulation's fidelity where nobody looks, because "only few places in the artificial world [are]
important at a given simulation time") is Brom, Šerý and Poch's ([IVE-LOD 2006](https://artemis.ms.mff.cuni.cz/main/papers/IVE-LOD-2006.pdf),
[IVA 2007](https://www.springerprofessional.de/en/simulation-level-of-detail-for-virtual-humans/2882154)).

### 2.2 Tiers (under model D)

| Tier | What exists | Rate | Where (per observer) | Scales with s? |
|---|---|---|---|---|
| **T0 Live** | Individual entities with physics, animation, full AI | Every fixed step | Within ~1-2 km on a surface, ~tens of km for ships in space (quality setting) | No: meters |
| **T1 Reduced** | Individuals, no physics or animation, coarse movement on schedules or nav graphs | Every Nth step, staggered by id | T0 radius to a few km (quality setting) | No: meters |
| **T2 Aggregate** | No individuals. Numbers per sim cell: density per species, stock per station, influence per faction | One coarse tick per period (domain-defined), aligned to the universal clock | Cells of the loaded body, the current system, plus the strategic active set | Cell count per body is fixed; cell area follows s_p² |
| **T3 Dormant** | Nothing runs. A persisted delta with its `last_tick`, or nothing at all (the baseline) | Never | Everywhere else | No |

Hitman used the same shape for crowds: 1,200 characters per zone, 500 on screen, tiers by distance, and a pool of
invisible NPCs respawned near the player (Intel deck, indicative; [PDF](https://www.intel.com/content/dam/develop/external/us/en/documents/06-create-a-scalable-and-destructible-world-in-hitman2-807276.pdf)).
Space Engineers throttles block update timers by "player presence" and "grid presence"
([Rosa 2020](https://blog.marekrosa.org/2020/08/space-engineers-server-optimizations/)), which is our T0/T1 split.

**Promotion** (T3 → T2) happens before the observer arrives: the prefetch distance is approach speed × lead time,
from Q10's travel tiers. Catch-up runs at promotion, not when the player lands. **Demotion** uses hysteresis (a
larger radius out than in) so a player on the boundary doesn't thrash.

### 2.3 Catch-up techniques, in order of preference

Each domain must implement `advance(state, from_tick, to_tick)` with the first technique that fits:

1. **Clock-derived (no state).** The value is a pure function of seed and tick: weather, eruption schedules, NPC
   ship voyages on schedules, herds' daily routines, seasons. Catch-up is free and multiplayer needs no sync.
   Precedent: on-rails orbits (Q2), Radiant AI's daily schedules made into functions of time ([UESP](https://en.uesp.net/wiki/Oblivion:Radiant)).
2. **Closed form.** An exact formula over elapsed time: logistic recovery of a harvested population,
   N(t) = K / (1 + ((K − N₀)/N₀)·e^(−rt)); regrowth timers; relaxation toward a baseline. Precedent: idle-game
   offline progress ([Math of Idle Games](https://gamedeveloper.com/design/the-math-of-idle-games-part-i)).
3. **Event-driven (piecewise).** Rates are constant between events, so jump from event to event: a base's
   production graph until the next buffer fills or empties. Cost is the number of events, not ticks. Factorio's
   "sleep when blocked" is the per-entity form ([forum](https://forums.factorio.com/viewtopic.php?p=505768)).
4. **Bounded coarse stepping.** Run the same T2 step function the live T2 tier runs, N times, with a cap. For
   coupled domains with no closed form (factions, economy). Precedent: Elite's daily tick; idle games simulating
   missed ticks fast.
5. **Snap to baseline.** When the elapsed time exceeds several relaxation times, the state *is* the baseline:
   delete the delta. This is what keeps saves from growing forever.

The rule that makes catch-up trustworthy: **running a domain dormant from t₀ to t₁ and then catching up must give the
same bits as running it at T2 the whole time.** Techniques 1, 2, 3 and 5 achieve that by construction if the T2 tier
uses the same formula. Technique 4 achieves it if catch-up calls the T2 step function and nothing else. Every domain
gets a CI test that checks it (section 3.3).

### 2.4 Per domain

| Domain | Options | Precedent | Pick |
|---|---|---|---|
| **Fauna** | (i) spawn on demand, no population; (ii) aggregate density per cell, individuals materialized near observers; (iii) every individual simulated | (i) NMS (community reports, unverified, [fan wiki](https://nomanssky.fandom.com/wiki/Fauna)); (ii) AC Unity recycling, Hitman pool; (iii) Eco | **(ii)**. Density per (cell, species) as a deviation from the generated carrying capacity K. Default state is K, so nothing is stored. |
| **Population model** | Lotka-Volterra oscillation; logistic recovery with predation folded into K; agent-based | LV in [The Reef](https://jakemchugh1.itch.io/the-reef), [Red Blob Games](https://www.redblobgames.com/dynamics/predator-prey/); LV reported "too unstable" for games (unverified) | **Logistic recovery toward a generated equilibrium.** Classic LV is neutrally stable (cycles drift with any error or perturbation) and has no closed form. Predator-prey ratios belong in Q3's generated K values. Oscillating models can be a module that uses bounded stepping. |
| **Flora** | Procedural placement only; placement plus persisted harvest with regrowth; simulated growth and spread | NMS placement; Eco's simulated plants | **Placement plus harvest deltas** (removed instance ids with a regrowth tick, deleted when regrown). Growth and spread (forests recovering from fire or lava) is a (b) extension: a cover fraction per cell that relaxes back. |
| **NPCs** | All ambient; ambient plus opt-in persistent; all persistent | Kenshi's persistent vs homeless squads; Oblivion's LLP flag; Star Citizen Quanta | **Ambient by default, persistent opt-in** (a named trader, a faction leader, a tamed pet). Persistent agents carry their own record and catch up by schedule or by the strategic tick. |
| **Factions** | Per-system influence numbers on a periodic tick; agent-based | Elite BGS daily tick with player activity batched in ([fan wiki](https://elite-dangerous.fandom.com/wiki/Influence)); DF armies; Stellaris, Bannerlord (no tick detail found) | **Influence per (system, faction) on a strategic tick**, Elite-style, run only inside the active set (section 2.5). Player actions are recorded as activity and applied at the next tick. Off-screen conflicts resolve by one aggregate rule seeded by the encounter id. |
| **Economy** | Every ship and station simulated (X4); station stocks and flows as numbers; player-only (EVE: NPC faucets and sinks on a player economy, [MassivelyOP](https://massivelyop.com/?p=50267)) | X4; Elite; EVE | **Station stocks, production and consumption as numbers**, prices from stock against target, flows as voyages. A reference module, replaceable. |
| **Traffic** | Real ships simulated cheaply (X4); spawn near player from a density; scheduled voyages | X4; Star Citizen's Quanta "without pathfinding, animations, or geometry" ([wiki](https://starcitizen.tools/Quantum_(game_system)), planned) | **Scheduled voyages** {origin, destination, departure tick, arrival tick, ship seed, cargo}. Position is a function of the clock along the Q10 travel model, like an on-rails orbit. Materialized as a physical ship when inside an observer's T0 radius. Ambient traffic with no economic role spawns from a route density, like fauna. |
| **Automated bases** | Full per-tick (Factorio); throttled ticks (SE); closed form (idle games); event-driven piecewise | as listed | **Event-driven piecewise-linear solver** over a declared production graph (rates, capacities, power). Exact, and cheap regardless of elapsed time. |
| **Weather** | Fluid simulation; per-planet random timers; a pure function of (position, tick) | NMS storms as per-planet events ([Origins](https://nomanssky.com/origins-update/)) | **A pure function.** Q3's static seasonal climate fields plus seeded storm systems (start tick, track along prevailing winds, radius, intensity) plus small-scale seeded noise. No state, no catch-up, no sync. |
| **Eruptions** | Visual only; scripted events; seeded events that write terrain edits | NMS volcanoes (visual hazard) | **Seeded point process per volcano** (event k's time from hash(volcano, k)) whose terrain effect is a deterministic list of brush operations along Q3's drainage graph. It enters the Q11 edit log as a system edit, ordered by event tick with player edits, when the region is next loaded or live if observed. |

### 2.5 The strategic active set

Factions and economies are coupled: one system's war changes its neighbor's prices. Lazy catch-up of one system with
frozen neighbors is an approximation. That's acceptable far from everyone, but not near players. So the strategic
layer runs as model C inside a bounded set:

- **Active set** = systems with a persisted delta that hasn't relaxed + systems within k jumps of any observer +
  a creator-defined core region (an "inhabited bubble", as in Elite).
- Inside the set, the strategic tick runs on schedule with bounded coarse stepping. Outside it, systems are frozen
  at `last_tick` (or at baseline). When one re-enters, it catches up against its neighbors' current values.
- The result is deterministic given the inputs, but **not independent of the player's path**: where you went
  decides which systems were active. Clock-derived and closed-form domains are path-independent; strategic domains
  are not. We should say this to creators plainly (product decision P7).

### 2.6 Materialization and fold-back

The protocol between T2 numbers and T0/T1 individuals is the heart of the design:

- **Identity by slot.** Ambient individual j of species s in cell c has seed hash(universe, "fauna", c, s, j). Live
  count = density × area inside the bubble. The first n slots by hash rank are live, so when density drops from 10
  to 9, the same 9 animals remain. Position comes from the slot's schedule (home range from the hash, activity by
  time of day), so two visits at the same tick see the same animal in the same place.
- **Fold-back is conservative.** When individuals dematerialize, only **player-caused, declared perturbations**
  change the aggregate: killed, tamed, captured, harvested, robbed, destroyed. Killed slots are stored per cell
  with a respawn tick, so the animal you killed doesn't come back while a different one disappears. Everything else
  the live simulation did (wandering, NPC-on-NPC predation, a ship's detour) is discarded, because the aggregate
  already accounts for it statistically.
- **Why this rule matters:** it makes T0 a *presentation* of T2. Then the T0 radius can be a quality setting (small
  on M1, large on M3 Pro), and the world's history is identical on both. It also avoids X4's problem, where the same
  fight has different outcomes in and out of the bubble: an encounter is resolved either live, because the player
  is in it, or by the aggregate rule, never by both.
- **Ships.** A voyage materializes with position and velocity from its schedule, so there is no pop at the
  boundary. If the player interacts (docks, attacks, trades), the voyage record takes the result (cargo, hull,
  re-planned route) and becomes a persisted delta until it arrives.

## 3. Tradeoffs

### 3.1 Overall models

| | A Freeze | B Full sim | C Continuous coarse | D Lazy, clock-derived (+C in active set) |
|---|---|---|---|---|
| **Complexity** | Low | Low per system, impossible at scale | Medium; two rule sets to keep in agreement | High up front: every domain needs a catch-up and fold-back story. Low per new domain once the framework exists |
| **Performance** | Cost ∝ observed area | Cost ∝ universe | Cost ∝ simulated set (needs a bound) | Cost ∝ observed area + touched records + active set |
| **Determinism** | Trivial | Trivial in principle, but needs all of it to run | Deterministic if the coarse tick is; X4-style drift between tiers | By construction, tested by live-vs-catch-up equivalence |
| **Extensibility** | Anything goes, nothing happens off-screen | Anything goes | New domains need low-attention rules | New domains must be declarative or provide `advance()`. That constrains creators (bases must declare rates) |
| **Testability** | Easy | Slow | Medium | Good: equivalence tests per domain, `advance_clock` over MCP, aggregates as reflected components |
| **Scale range** | Fine | Breaks first | Bounded set holds at any s | Holds: fixed cell count per body, density stored per area |

### 3.2 Specific tradeoffs

- **Declarative machines vs scripted machines.** Closed-form base production needs machines that declare rates
  and capacities as data. A machine with arbitrary per-tick Luau logic can't be solved and must be stepped. The
  ruling: **declarative machines run off-screen; scripted machines run only at T0/T1** and declare an average rate
  that applies off-screen. This is a real restriction on creators, and the price of bases that work while the owner
  is away.
- **Weather as a function vs simulated weather.** A function can't respond to the world (terraforming doesn't
  change the climate, smoke from an eruption doesn't shade the region unless the eruption's record feeds the
  function). The gain: zero state, zero sync, perfect multiplayer agreement, free catch-up. Responsive climate is (c)
  for now.
- **Aggregates as ECS entities vs a separate store.** Flecs entities give reflection, MCP, Luau and replay hashing
  for free, at ~0.5-1 ns per entity per component to iterate ([ecs_benchmark](https://github.com/SanderMertens/ecs_benchmark)).
  A separate SoA store is faster and invisible to tools. **Pick entities for T2 records that are loaded** (the
  current body's cells, the active set); dormant records live only in the delta store (Q11), not in the world.
- **Replay hash cost.** Record/replay hashes every Scriptable component every frame. With 10⁴-10⁵ aggregate
  records that becomes expensive. Propose to the record-replay spec: hash aggregate records only on steps where a
  coarse tick ran, or keep an incremental per-record hash updated on write.
- **Integer vs float aggregates.** Authoritative aggregate state (populations, stocks, influence) as integers or
  fixed point (influence in per-mille, like Elite's percentages) hashes identically on ARM and x64. Floats need Q12's
  deterministic math for every `exp` in a relaxation formula. Integers are more work to write; floats are a bug
  waiting for the first Windows CI comparison.

### 3.3 Determinism and threading rules (engineering, proposed as ADRs)

1. **Simulation time is the universal tick** (int64, Q2). Coarse schedules never read `Time.elapsed` (a double)
   and never use Flecs `.interval()`, which accumulates float seconds. Flecs `.rate(N)` is deterministic but counts
   from system creation, not from the clock, so it misaligns after loading a save.
2. **Coarse ticks fire when `tick % period == stagger(id)`**, with stagger = hash(id) mod period. This spreads load
   evenly over steps (which matters while single-threaded) and stays aligned across save and load.
3. **Counter-based randomness:** `rand(universe_seed, domain, id, tick, draw)` as a hash, in the style of Squirrel
   Eiserloh's noise-based RNG ([GDC 2017](https://www.youtube.com/watch?v=LWFzPP8ZbdU); use the revised
   SquirrelNoise5 or a SplitMix/PCG hash, not the talk's original Squirrel3). The result is independent of call order,
   thread and live vs catch-up. It belongs in the engine (generic, [ADR 0042](../adr/0042-space-toolbox-layering.md)).
   The `Random` singleton stays for scripts and tests; simulation LOD never uses it.
4. **Authoritative aggregate state is integer or fixed point.** Any transcendental in a rule goes through Q12's
   deterministic math or a table.
5. **A coarse step reads the previous tick's state and writes only its own record.** Cross-record effects
   (migration, trade, attacks) are messages, sorted by (target, source, sequence) and applied at the next tick. That
   makes every aggregate tick an order-independent parallel-for, ready for VCH-015 without changing results.
6. **Live-vs-catch-up equivalence** for every domain: a CI test runs a domain at T2 for N ticks, and separately
   dormant then `advance()`, and compares state hashes. Headless, null RHI.
7. **Tier changes happen at tick boundaries.** What a materialized cell contains is a pure function of the
   aggregate, the tick and the seeds. Only *which* cells are materialized depends on observers.

**Threading now and later.** Until VCH-015, everything runs on the main thread: staggering (rule 2) keeps coarse
ticks flat; closed-form catch-up costs microseconds per record; bounded stepping spreads over several frames while a
region is still being promoted (it's beyond T0 anyway). After VCH-015: aggregate ticks become parallel-for jobs
(rule 5), and catch-up becomes an async job whose result applies at a defined tick, the same "ready" pattern as Q4's
terrain chunks.

**Luau.** Domain rules may be Luau, run as batched functions over arrays of records (one C++→Luau call per coarse
tick per domain, as in [ADR 0007](../adr/0007-scripting-luau.md)). The catch-up loop always stays in C++ and calls
the Luau step with a step cap, because Luau has no interrupts yet. A Luau interrupt/budget is a dependency
(flagged in ADR 0007's consequences).

**Testability over MCP.** Proposed tools: `sim_tiers` (which cells and agents are in which tier, per observer),
`advance_clock {ticks}` (headless only: time passes for dormant regions through the catch-up path, so an agent can
test "come back after a year"), and aggregate records readable with the existing `query_entities` and
`get_component` because they are reflected components.

## 4. Extension points

| What gets added | Luau | C++ module | Toolbox core |
|---|---|---|---|
| New species, flora kinds, recipes, station types, faction archetypes, storm kinds, volcano styles | **Data + rules** | — | — |
| A domain rule (how a population responds, how influence spreads, how prices move) | **Batched step function** over records, deterministic math from `@veach` | Same, faster | — |
| A whole new simulated domain (disease, migration, religion, pollution) | Possible if expressed as records + a step rule + a declared catch-up technique | **Implements the `SimDomain` interface**: records, step, `advance()`, materializer, fold-back | — |
| A new materializer (how a herd or a convoy becomes entities near players) | Spawn hook that receives slot seeds | **Materializer** | — |
| Machines for automated bases | **Declarative** (rates, capacities, power) | Custom solvers for new machine classes | — |
| Scripted machines with per-tick logic | Yes, T0/T1 only, with a declared off-screen average | Yes | — |
| Storyteller / event director (RimWorld-style raid points from wealth, [wiki](https://www.rimworldwiki.com/wiki/Raid_point)) | **Event generator** over aggregates and the clock | Yes | — |
| Tiers, scheduler, counter-based RNG, delta store, fold-back invariant, equivalence test harness | — | — | **Core** (scheduler and RNG move down into the engine) |

The rule for creators: **a new kind of thing is data plus a registered rule; a new kind of simulation is a module
that implements `SimDomain`**. Nothing is an enum in the core.

## 5. Interactions with the other questions

| Q | Interaction |
|---|---|
| **Q1 coordinates** | Observer positions in frames drive tier assignment; T0/T1 radii are meters in the body-local frame; voyages live in the system frame; cells are keyed in body-local coordinates. |
| **Q2 time** | The int64 universal tick is the simulation clock. Voyages and schedules are the same pattern as on-rails orbits. Seasons and time of day drive herd routines and climate. **Time warp is the catch-up path**: during warp everything runs at T2 or above, with no live individuals. |
| **Q3 generation** | Provides every baseline: carrying capacity per cell, species lists, factions, stations and recipes, volcano list and activity (from internal heat), climate fields, the drainage graph that lava follows. Seed-hierarchy ids key the RNG. |
| **Q4 terrain** | A "chunk ready" hook to place materialized individuals on the surface; eruption edits go through the terrain edit pipeline; the per-body cell grid should share Q4's cube-sphere quadtree at a fixed level. |
| **Q5 procgen graph** | Baselines come out of the graph. Simulation rules are not graph nodes (they depend on time), but use the same Luau hook mechanism and deterministic math. |
| **Q6 networking** | The server runs the strategic layer and catch-up. Clients regenerate clock-derived things locally (weather, voyages, eruption schedules) with no traffic. Only deltas and materialized agents are synced. Interest management *is* tier assignment over the union of observers. |
| **Q8 renderer** | The weather field drives clouds and precipitation (a visual-only GPU copy is fine; gameplay reads the CPU function, Q12). T1 agents can render as cheap instances. |
| **Q9 physics** | Physics only at T0. Ships hand off from voyage to rigid body with continuous position and velocity. |
| **Q10 travel** | Approach speed sets the promotion distance. An FTL arrival triggers the destination system's catch-up behind the jump transition. |
| **Q11 persistence** | The delta store is part of the save: records keyed by (domain, id), with `last_tick` and rule version. **Simulation rules are versioned like generators**, because changing an ecosystem rule changes catch-up results. Eruption edits enter the edit log as system edits. |
| **Q12 determinism** | Integer/fixed-point aggregates, deterministic math in rules, a counter-based RNG that is bit-identical on ARM and x64. The cross-platform CI hash test should include the equivalence tests. |

## 6. M1-class vs M3 Pro-class implications

The base M1 has 4 performance + 4 efficiency cores. The M3 Pro has 6P+6E (or 5P+6E), with single-thread
performance "up to 30% faster than M1 Pro" by Apple's numbers
([Apple](https://www.apple.com/newsroom/2023/10/apple-unveils-m3-m3-pro-and-m3-max-the-most-advanced-chips-for-a-personal-computer/),
[Eclectic Light](https://eclecticlight.co/2023/12/12/evaluating-the-m3-pro-summary/)). Until VCH-015 we use one core,
so the gap is about 1.3×. With the job system, aggregate ticks parallelize and the gap grows to roughly 2× or more.

Budget at 60 Hz, simulation LOD's share of the main thread, **estimates to be replaced by spike S7.3's
measurements** (unverified):

| Item | M1-class | M3 Pro-class | Notes |
|---|---|---|---|
| T0 live agents (AI + steering + nav, physics separate) | ~150-300 | ~200-400 single-threaded; more with jobs | The dominant cost. Quality setting. |
| T1 reduced agents at 1/8 rate | ~1-2 k | ~2-4 k | Quality setting. |
| T2 records per coarse tick (C++ rule) | ~10⁴-10⁵, staggered | same, more headroom | Luau rules perhaps 5-20× slower (unverified). |
| Strategic active set | ~10⁴ systems | same or larger | Each system a few faction and station records. |
| Catch-up at promotion | Closed form: µs per record; a planet's cells × domains in a few ms | same | Bounded stepping is capped and spread over frames. |
| Delta store memory | Grows with what players touched; MB scale for heavy play | same | Relaxation deletes deltas. |

The important consequence: **because of the fold-back rule, the T0 and T1 limits are quality settings that change
how many creatures you see near you, never what happens in the world.** An M1 player sees a smaller herd around
them. The herd's numbers, the faction war and the base inventory are identical to the M3 Pro player's. A design
where live simulation can change aggregates would make the hardware target a gameplay decision; this one keeps it
a presentation decision.

## 7. Recommendation

**Adopt model D.** Concretely:

1. **State contract:** world state = generated baseline (Q3) + persisted deltas (Q11) + functions of the universal
   tick (Q2). A record with no delta *is* the baseline and costs nothing.
2. **Four tiers** (Live, Reduced, Aggregate, Dormant) per observer, with hysteresis, promotion ahead of the observer
   by Q10's approach speed, T0/T1 radii in meters as quality settings, and a fixed sim-cell count per body.
3. **Catch-up order:** clock-derived, then closed form, then event-driven, then bounded coarse stepping, then snap
   to baseline. Every domain declares which it uses and passes a live-vs-catch-up equivalence test in CI.
4. **Fold-back invariant:** only player-caused, declared perturbations flow from live individuals into aggregates.
5. **Strategic active set** for coupled domains (factions, economy), with a creator-defined core region.
6. **Determinism rules** in section 3.3: tick-aligned staggered schedules, a counter-based RNG in the engine,
   integer or fixed-point aggregates, double-buffered message-passing aggregate ticks ready for the job system.
7. **Domain picks:** logistic recovery toward generated equilibria for fauna; placement plus harvest deltas for
   flora; ambient NPCs with opt-in persistent ones; Elite-style influence ticks for factions; station stocks with
   voyages for economy and traffic; a piecewise-linear solver over declarative machines for bases; weather as a pure
   function; eruptions as seeded events that write system edits.
8. **The core ships the framework and one reference implementation per domain**; each domain is a module that
   creators can replace.

**What would change my mind:**

- If the owner wants Eco-style ecology, where individual creatures and plants evolve and go extinct from emergent
  interactions, D's "individuals are a presentation" rule is wrong. That needs a server-scale individual simulation
  on a bounded world, which is a different product.
- If multiplayer becomes a persistent MMO-style server that runs all the time, the strategic layer runs
  continuously (model C, like Elite's servers), and lazy catch-up matters only for single player.
- If spike S7.1 shows bounded stepping can't match live T2 bit-for-bit in practice (float leaks, ordering bugs we
  can't contain), I'd drop bounded stepping for strategic domains and keep the active set always ticking, accepting
  frozen systems outside it.
- If S7.3 measures T2 records an order of magnitude cheaper than estimated, the active set should grow, possibly to
  every system the player has visited.

## 8. What is expensive to undo

- **The state contract** (baseline + delta + clock). Every save and every wire format depends on it.
- **RNG keying.** Changing the hash or the key layout reshuffles every creature, voyage and storm in every universe.
  That's a generator-version bump under Q11, with old versions kept runnable for saves.
- **Integer/fixed-point formats** of aggregate records: saves and wire.
- **The declarative machine contract.** Creators' base content will be written against it.
- **The fold-back invariant.** Loosening it later makes outcomes depend on the hardware's T0 radius.
- **Per-body sim cell grid.** Cell ids are keys in saves; changing the level or layout needs a migration.
- **Coarse periods and step rules of stepped domains.** They change results, so they are part of the rule version,
  not free tuning knobs.

Cheap to change: tier radii and agent caps (quality settings), hysteresis, individual domain models behind
`SimDomain`, MCP tool shapes.

## 9. Proposed ADR titles

1. Simulation LOD: four tiers per observer and a fixed sim-cell grid per body
2. World state is generated baseline plus persisted deltas plus functions of the universal clock
3. Off-screen catch-up order: clock-derived, closed form, event-driven, bounded stepping, snap to baseline
4. Live-vs-catch-up equivalence is tested in CI for every simulated domain
5. Counter-based random streams keyed by domain, id and tick (engine)
6. Coarse simulation ticks aligned to the universal clock and staggered by id (engine)
7. Authoritative aggregate state is integer or fixed point
8. Materialization by slot identity, and fold-back of player-caused perturbations only
9. Strategic active set for factions and economy
10. Automated bases are declarative production graphs solved piecewise-linearly; scripted machines run only live
11. Weather is a function of position and tick; eruptions are seeded events that write system edits

## 10. Spike candidates

Each is the smallest throwaway prototype that validates a decision. Sized by scope.

| Id | Validates | Scope | Visible success criterion |
|---|---|---|---|
| **S7.1 Catch-up equivalence** | ADRs 2-7 | One throwaway lab module: a toy population per cell (logistic with a harvest perturbation) and one two-machine production chain; a counter-based RNG function; a tick-aligned staggered scheduler. Headless, null RHI, no persistence format, no rendering. | Three runs (live T2 for N ticks; dormant then `advance()`; dormant past relaxation then snap) print identical state hashes, checked over MCP. Tracy shows catch-up of one in-game year for 6,144 cells under 1 ms on the M5 Max. |
| **S7.2 Materialize and fold back** | ADR 8 | Flat lab plane (no planet, to avoid depending on Q4), a grid of cells with densities, individuals as colored cubes driven by slot schedules, a key to "kill" the one in front of the camera, the fly camera as observer. | Screenshots before and after flying out of range, advancing the clock and returning: untouched slots are the same cubes at their schedule positions; killed ones are absent until their respawn tick. Changing the T0 radius leaves the aggregate hash unchanged. Tier transitions under 0.5 ms per frame in Tracy. |
| **S7.3 Tier cost table** | Section 6 budgets | Same module, N agents at each tier, C++ and Luau versions of the T2 rule. | A measured table of µs per agent per tier, and Luau vs C++, on the M5 Max, scaled to M1 and M3 Pro by single-core ratios. It replaces the estimates in section 6. |
| **S7.4 Scheduled voyages** | Traffic pick, Q9 handoff | Two stations in a toy system, N scheduled voyages, materialization inside a radius. | A screenshot sequence across the boundary shows no pop; logged position error at handoff under 1 m with continuous velocity. |
| **S7.5 Eruption as a system edit** (after Q4 and Q11 spikes) | ADR 11 | One volcano on the terrain spike's planet with a seeded schedule. | Watching an eruption live and arriving after it by `advance_clock` produce the same lava deposit (`veach imgdiff` of the two screenshots passes). |

Order: S7.1 first (it validates the contract everything else rests on), then S7.3 and S7.2. S7.4 when Q10's travel
model exists; S7.5 after the terrain and edit-log spikes. None of them is on the first slice's critical path. The
first slice needs only the clock and seasons from Q2, which this design reuses.

## 11. Product decisions surfaced

| # | Decision | Recommendation |
|---|---|---|
| P1 | Does the universe change while nobody is looking? | **Yes, lazily.** Clock-driven things (weather, eruptions, traffic, herd routines) always continue; perturbations relax back; factions and economy evolve inside the active set. |
| P2 | Is ecological damage permanent (extinction)? | **Recovery by default**, with a creator setting for recovery rate, including "never" for Eco-style permanence. |
| P3 | Can eruptions and storms damage player bases? | **Storms yes, only while observed. Eruptions never edit inside claimed base volumes by default**; creator setting to allow it. |
| P4 | Offline base production: full or capped? | **Full accrual, limited by storage, inputs and power**, no artificial cap; creators may add one (Egg, Inc. caps offline earnings). |
| P5 | Are NPCs persistent individuals? | **Ambient by default, persistent opt-in** per NPC kind. |
| P6 | Time warp (with Q2) | **Single player only, and implemented as catch-up**: no live individuals during warp. |
| P7 | What determinism do we promise creators? | **"Same inputs, same world"**: path-independent for clock and closed-form domains, path-dependent for the strategic layer. Say so in the docs. |
| P8 | Hardware target interplay | T0/T1 radii and caps are quality settings with M1 and M3 Pro presets. They never affect outcomes (fold-back rule). |

## 12. Sources

Full annotated list with quotes and reliability notes: [notes/07-simulation-lod-notes.md](notes/07-simulation-lod-notes.md).

Games and talks:

- X4: Foundations store page: https://store.steampowered.com/app/392160/X4_Foundations/; high/low attention discussion
  (developer and players): https://steamcommunity.com/app/392160/discussions/0/6664812048260991709; patch notes
  (summary, not fetched): https://steamdb.info/patchnotes/7350010
- Elite Dangerous BGS: https://community.elitedangerous.com/en/node/362 (2016 dev update, now redirects),
  https://forums.frontier.co.uk/threads/when-and-what-is-the-tick.400292/,
  https://elite-dangerous.fandom.com/wiki/Influence,
  https://forums.frontier.co.uk/threads/elite-dangerous-action-taken-to-address-recent-bgs-issues.611225,
  https://yesxorno.substack.com/p/ed-bgs-and-pp2
- Star Citizen StarSim/Quantum (planned): https://starcitizen.tools/Quantum_(game_system), https://starcitizen.tools/StarSim;
  OCS: https://starcitizen.tools/Object_Container_Streaming
- Dwarf Fortress: https://dwarffortresswiki.org/index.php/World_activities, https://dwarffortresswiki.org/Army,
  https://bay12games.com/dwarves/dev.html,
  https://www.gamedeveloper.com/design/-i-dwarf-fortress-i-figuring-out-how-to-simulate-the-universe-one-step-at-a-time
- RimWorld (community wiki): https://www.rimworldwiki.com/wiki/Raid_point, https://rimworldwiki.com/wiki/AI_Storytellers
- EVE Online: https://massivelyop.com/?p=50267, https://www.eveonline.com/news/view/monthly-economic-report-march-2025,
  https://www.eveonline.com/de/news/view/monthly-economic-report-may-2026
- Bannerlord campaign AI: https://gamebanshee.com/3wfc8
- Kenshi (player reports): https://steamcommunity.com/app/233860/discussions/0/3276942988970004699
- Oblivion: https://en.uesp.net/wiki/Oblivion:Radiant, https://forums.nexusmods.com/topic/277365-ai-wont-leave-house
- No Man's Sky: https://nomanssky.com/origins-update/, https://nomanssky.fandom.com/wiki/Fauna (community),
  https://steamcommunity.com/app/275850/discussions/0/3278065083967257005 (community)
- Space Engineers: https://blog.marekrosa.org/2020/08/space-engineers-server-optimizations/,
  https://spaceengineerswiki.com/Trash_Removal, https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/
- Factorio: https://factorio.com/blog/post/fff-204, https://updater.factorio.com/blog/post/fff-421,
  https://forums.factorio.com/viewtopic.php?p=505768, https://forums.factorio.com/viewtopic.php?p=588684
- Eco: https://pcgamer.com/eco-is-an-open-world-survival-game-that-will-let-you-destroy-the-planet,
  https://wikipedia.com/wiki/Eco_(2018_video_game)
- Hitman crowds, GDC 2012: https://gdcvault.com/play/1015315/Crowds-in-Hitman; Intel deck (summary, indicative):
  https://www.intel.com/content/dam/develop/external/us/en/documents/06-create-a-scalable-and-destructible-world-in-hitman2-807276.pdf
- Assassin's Creed Unity AI recycling, GDC 2015: https://www.gdcvault.com/play/1022141/Massive-Crowd-on-Assassin-s,
  https://game.watch.impress.co.jp/docs/news/20150307_691715.html,
  https://www.gamespot.com/articles/assassins-creed-unity-can-support-crowds-of-30-000/1100-6421542/
- Idle games: https://gamedeveloper.com/design/the-math-of-idle-games-part-i,
  https://blog.clickerheroes.com/offline-progression-in-clicker-heroes/

Research and techniques:

- Brom, Šerý, Poch, simulation LOD: https://artemis.ms.mff.cuni.cz/main/papers/IVE-LOD-2006.pdf,
  https://www.springerprofessional.de/en/simulation-level-of-detail-for-virtual-humans/2882154; LOD AI (Augsburg):
  https://opus.bibliothek.uni-augsburg.de/opus4/files/46021/46021.pdf
- Lotka-Volterra in games: https://jakemchugh1.itch.io/the-reef, https://www.redblobgames.com/dynamics/predator-prey/,
  https://odr.chalmers.se/server/api/core/bitstreams/de3d3f4f-9b33-4b8e-80d9-71845b6d6d10/content
- Squirrel Eiserloh, Noise-Based RNG, GDC 2017: https://www.youtube.com/watch?v=LWFzPP8ZbdU
- Floating-point determinism: https://gafferongames.com/post/floating_point_determinism/

Hardware and benchmarks:

- Apple M3 family: https://www.apple.com/newsroom/2023/10/apple-unveils-m3-m3-pro-and-m3-max-the-most-advanced-chips-for-a-personal-computer/
- https://eclecticlight.co/2023/12/12/evaluating-the-m3-pro-summary/,
  https://www.notebookcheck.net/M1-vs-M3-vs-Apple-M3-Pro-12-Core_12937_15110_15115.247596.0.html
- Flecs benchmarks: https://github.com/SanderMertens/ecs_benchmark, https://github.com/abeimler/ecs_benchmark

Repository: [ADR 0005](../adr/0005-ecs-flecs-v4.md), [0006](../adr/0006-flecs-barrel-not-wrapper.md),
[0007](../adr/0007-scripting-luau.md), [0018](../adr/0018-engine-lifecycle-and-phases.md),
[0019](../adr/0019-veach-modules-and-services.md), [0042](../adr/0042-space-toolbox-layering.md),
[record-replay spec](../specs/record-replay/spec.md), `docs/guides/flecs-v4/upstream/Systems.md` (rate, interval,
tick sources), `engine/core/include/veach/core/rng.hpp`, `engine/ecs/include/veach/ecs/components.hpp`.

**Unverified, collected:** X4's bubble radius and low-attention rules beyond combat; NMS creature spawning; Kenshi
and Oblivion off-screen behavior (player and modder reports); Elite's tick schedule (community trackers); Star
Citizen StarSim status (planned); Hitman LOD tier counts (secondary summary of a deck); the "LV too unstable"
report; every per-agent cost and count in section 6.
