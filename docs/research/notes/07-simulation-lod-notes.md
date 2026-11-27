# Q7 Simulation LOD: raw notes

Researcher notes for [../07-simulation-lod.md](../07-simulation-lod.md). Date 2026-11-27. Web searches were run with
summarized results; pages marked "fetched" were read directly. Community wikis and forum posts are marked as such;
anything without a source is in Gaps.

## Repo facts this question builds on (read from the tree, not the web)

- Fixed 60 Hz simulation phases (Simulation → PostSimulation → TransformPropagation), one Flecs pipeline per phase,
  run by `PhaseRunner` with `world.run_pipeline(pipeline, delta_seconds)` (`engine/ecs/src/phases.cpp:81`). Windowed
  runs take at most 8 fixed steps per frame and drop the backlog after a stall; headless runs take exactly one step
  per frame ([ADR 0018](../../adr/0018-engine-lifecycle-and-phases.md)).
- `Time` singleton: `frame`, `fixed_step` (uint64), `delta`, `elapsed` (double) (`engine/ecs/include/veach/ecs/components.hpp`).
- `Random` singleton: **one** PCG32 (`Rng`, `engine/core/include/veach/core/rng.hpp`) seeded from `--seed`. PCG32
  supports a stream selector, but the engine exposes a single shared stream, so every draw depends on the order of
  every earlier draw.
- Flecs v4.2 offers `.rate(N)` (every Nth run of the pipeline) and `.interval(seconds)` (accumulates the delta it is
  given), plus shared tick sources (`docs/guides/flecs-v4/upstream/Systems.md`, "Interval", "Rate", "Tick source").
- Everything is single-threaded until the job system lands (VCH-015, enkiTS or Taskflow by ADR; Flecs workers hooked
  into it). [ADR 0018](../../adr/0018-engine-lifecycle-and-phases.md) consequences: "Flecs threading must later be
  hooked into our own job system."
- Luau script systems get their whole query result in one C++→Luau call; per-entity work runs inside the VM; writes
  go through a command buffer; no yielding, no interrupts or timeouts yet
  ([ADR 0007](../../adr/0007-scripting-luau.md)).
- Record/replay hashes every Scriptable component (FNV-1a over reflected serialization, in entity order) every frame
  ([record-replay spec](../../specs/record-replay/spec.md)).
- 00-scope (a) items for Q7: catch-up from the clock (closed-form or event-sourced, not stepping); aggregate
  ecosystems with individuals near players; economy, traffic, factions; automated bases in closed form; weather and
  eruptions as system edits. Scale notes: density per m² fixed, total population ∝ s_p²; route lengths follow s_sys;
  storm sizes follow s_p ([00-scope.md](../00-scope.md) section 9).

## 1. LOD tiers, catch-up and what persists: precedents

### Takeaway
Every shipped precedent uses two to four tiers: full simulation in a bubble around the player, a cheaper rule set or
update rate outside it, abstract records for the rest, and either freezing or a coarse periodic tick for places
nobody watches. None publishes a rigorous "live equals catch-up" guarantee; the closest are Elite's daily aggregate
tick and DF's abstract armies.

### Cited Findings
- X4: Foundations claims a fully simulated economy that keeps running without the player: "They trade, mine, fight,
  produce, and expand, even when you're not there" — [Steam store page](https://store.steampowered.com/app/392160/X4_Foundations/)
- X4 uses "high attention" (full simulation inside a bubble around the player) and "low attention" (simplified
  simulation outside); an Egosoft developer (eMYNOCK) confirms the terms and that OOS/IS are the older names
  (fetched) — [Steam discussion](https://steamcommunity.com/app/392160/discussions/0/6664812048260991709)
- X4 low-attention combat skips line-of-fire checks and only tests range, so station turrets all fire at once and
  outcomes differ from high attention (player claim) — [same thread](https://steamcommunity.com/app/392160/discussions/0/6664812048260991709)
- X4 bubble size: "80km iirc" (player, from memory); a fan guide says 50-150 km and bubble = current zone plus
  neighbours; unverified, sources conflict — [same thread](https://steamcommunity.com/app/392160/discussions/0/6664812048260991709)
- X4 patch notes list "Improved low attention combat involving multiple capital ships" (4.10) and "Improved combat
  simulation when player is not present" (4.00), i.e. the off-screen rules were tuned for years after launch —
  [SteamDB patch notes](https://steamdb.info/patchnotes/7350010) (search summary, not fetched)
- Elite Dangerous background simulation (BGS): influence and faction states update on an approximately daily tick;
  the period's player activity is aggregated and applied as influence change at the tick — [Frontier dev update 2016](https://community.elitedangerous.com/en/node/362)
  (now redirects; content via search summary); [forum "When and what is the tick?"](https://forums.frontier.co.uk/threads/when-and-what-is-the-tick.400292/)
- Elite factions always have primary states; conflict states are triggered by relative influence and take priority
  over economic states; background simulation and player activity both drive states, players much more — [Influence (fan wiki)](https://elite-dangerous.fandom.com/wiki/Influence)
- Elite's tick time drifts and is detected by community trackers, not published; some low-activity systems appear to
  be skipped some days (anecdotal) — [Frontier forum](https://forums.frontier.co.uk/threads/when-and-what-is-the-tick.400292/);
  Powerplay aggregates weekly on top of the daily BGS tick — [Substack](https://yesxorno.substack.com/p/ed-bgs-and-pp2)
- Frontier once re-ran the BGS from earlier values after an influence-processing error — [Frontier forum](https://forums.frontier.co.uk/threads/elite-dangerous-action-taken-to-address-recent-bgs-issues.611225)
  (search summary). Implication: their aggregate sim is reproducible from stored inputs.
- Star Citizen's StarSim (formerly Quantum): Quanta are "lightweight NPCs" with "desires, skills, jobs, and
  inventory", run "without the overhead of things like pathfinding, animations, or even geometry"; "hundreds of
  thousands" of them; player activity is fed back in. Status reads as planned, not shipped; the page itself warns
  some info is over two years old (fetched) — [Star Citizen Wiki](https://starcitizen.tools/Quantum_(game_system))
- An older developer claim of ~20 million AI agents for Quantum (unverified, forum-quoted) — [Star Citizen Wiki StarSim](https://starcitizen.tools/StarSim)
- Star Citizen's Object Container Streaming freezes unobserved containers — [OCS (community wiki)](https://starcitizen.tools/Object_Container_Streaming) (cited in 00-scope)
- Dwarf Fortress: world generation plays out history by running the world's civilizations; since 0.40.01 "world
  activation" keeps the world developing during play (succession disputes, usurpations, armies moving) —
  [DF wiki World activities](https://dwarffortresswiki.org/index.php/World_activities) (search summary; fetch got 429)
- DF "army" is "an abstract data structure used to track moving people"; a group can be one person; invasions
  physically cross the world map — [DF wiki Army](https://dwarffortresswiki.org/Army); off-site army battles are
  still on the development list (last updated 2025) — [Bay 12 dev page](https://bay12games.com/dwarves/dev.html)
- Tarn Adams on scope: "we're more on a crusade to put the narratively interesting parts of the world... We don't
  care about atoms" (fetched) — [Game Developer](https://www.gamedeveloper.com/design/-i-dwarf-fortress-i-figuring-out-how-to-simulate-the-universe-one-step-at-a-time)
- Kenshi (player reports, unverified): "persistent" squads spawned from towns are stored in the save when you leave
  an area; "homeless" wilderness squads are deleted when they leave the loaded area; areas are rebuilt on re-entry —
  [Steam discussion](https://steamcommunity.com/app/233860/discussions/0/3276942988970004699)
- Oblivion's "Low Level Processing" flag keeps an NPC's AI and schedule running when the player is not in its cell;
  turning it off "kills your AI when you are not watching" (modder forum, unverified) — [Nexus forum](https://forums.nexusmods.com/topic/277365-ai-wont-leave-house);
  Radiant AI = daily schedules — [UESP](https://en.uesp.net/wiki/Oblivion:Radiant)
- Space Engineers "Selective Physics Updates" (optional, dedicated servers): "only worlds around the players on the
  server are simulated (physics)"; "Tiered Update": grids are in "grid presence" or "player presence" states, and
  blocks (reactors, thrusters, batteries, drills, refineries, assemblers, turrets...) change update timers with the
  state; main-thread load fell "from around 90% to 60%" on a test server (fetched) —
  [Marek Rosa, 2020](https://blog.marekrosa.org/2020/08/space-engineers-server-optimizations/)
- Space Engineers "stop grids" zeroes the velocity of grids far from players every N minutes (default 15) —
  [SE wiki Trash Removal](https://spaceengineerswiki.com/Trash_Removal)
- Factorio has no simulation LOD: every active entity updates every tick, and large bases are memory-bound ("every
  active entity is read each tick"); prefetching gave 9-13% — [FFF #204](https://factorio.com/blog/post/fff-204);
  2.0 found most roboports "only consume power" and need not update constantly — [FFF #421](https://updater.factorio.com/blog/post/fff-421)
- Factorio entities go to sleep when output is blocked and cost ~nothing until woken; fluid entities never sleep
  (forum, partly developer) — [forum p=505768](https://forums.factorio.com/viewtopic.php?p=505768),
  [forum p=588684](https://forums.factorio.com/viewtopic.php?p=588684)
- Hitman Absolution (GDC 2012, Kasper Fauerby): 1,200-character crowds at 30 fps on PS3/360 —
  [GDC Vault](https://gdcvault.com/play/1015315/Crowds-in-Hitman); an Intel deck on Hitman 2 lists 1,200 characters
  per zone, 500 max on screen, AI limited per zone, tiered LOD (~25 within 7 units, 75 within 15, 300 within 22),
  and a small pool of invisible NPCs respawned near the player for interactions (search summary of the PDF; fetch
  failed on size, so indicative) — [Intel PDF](https://www.intel.com/content/dam/develop/external/us/en/documents/06-create-a-scalable-and-destructible-world-in-hitman2-807276.pdf)
- Assassin's Creed Unity, "Massive Crowd on Assassin's Creed Unity: AI Recycling" (GDC 2015, Francois Cournoyer):
  an AI LOD technique with pooling that swaps low-resolution NPCs for high-resolution ones unnoticed —
  [GDC Vault](https://www.gdcvault.com/play/1022141/Massive-Crowd-on-Assassin-s); AI switched by distance, LOD
  reduces logic as well as animation — [Game Watch report](https://game.watch.impress.co.jp/docs/news/20150307_691715.html);
  claims of up to 30,000 crowd members — [GameSpot](https://www.gamespot.com/articles/assassins-creed-unity-can-support-crowds-of-30-000/1100-6421542/)
- Academic "simulation LOD": Brom, Šerý, Poch, "Simulation Level of Detail for Virtual Humans" (IVA 2007): reduce
  simulation quality where nobody watches; "there often exist only few places in the artificial world important at
  a given simulation time" — [IVE-LOD 2006 paper](https://artemis.ms.mff.cuni.cz/main/papers/IVE-LOD-2006.pdf),
  [Springer chapter](https://www.springerprofessional.de/en/simulation-level-of-detail-for-virtual-humans/2882154);
  "LOD AI" (Wißner, Kistler, André) lowers update rates and path planning detail by LOD —
  [Augsburg](https://opus.bibliothek.uni-augsburg.de/opus4/files/46021/46021.pdf) (abstracts only)
- RimWorld's storyteller spends "raid points" computed from colony wealth (buildings at half value), colonist count,
  threat scale and an adaptation factor; event type is rolled separately from points (community wiki) —
  [Raid points](https://www.rimworldwiki.com/wiki/Raid_point), [AI Storytellers](https://rimworldwiki.com/wiki/AI_Storytellers)
- EVE Online's economy is player-driven with NPC faucets and sinks reported monthly: one report's top faucets were
  NPC bounties (33.9 T ISK), commodity sales to NPC corps (18.2 T) and Incursion payouts (11.2 T) —
  [MassivelyOP](https://massivelyop.com/?p=50267); recent reports: [March 2025](https://www.eveonline.com/news/view/monthly-economic-report-march-2025),
  [May 2026](https://www.eveonline.com/de/news/view/monthly-economic-report-may-2026)
- Bannerlord: every party on the campaign map "has a function in the game's sandbox simulation"; lords prioritise
  actions by abilities and needs — [TaleWorlds blog via GameBanshee](https://gamebanshee.com/3wfc8) (no tick detail found)
- Idle games: offline progress is either full re-simulation (sometimes at a multiplied step), a closed form
  (rate × elapsed, often capped, e.g. Egg, Inc.'s cap) or a hybrid; closed form breaks when rates change during the
  offline period; re-simulation duplicates logic unless the same code runs both ways — [Math of Idle Games I](https://gamedeveloper.com/design/the-math-of-idle-games-part-i),
  [Clicker Heroes offline](https://blog.clickerheroes.com/offline-progression-in-clicker-heroes/) (search summary,
  community-heavy)

### Inferences
- Two families exist: **freeze** (Star Citizen OCS, SE stop grids, Kenshi area unload) and **keep going cheaply** (X4
  low attention, Elite daily tick, DF world activation, Oblivion LLP). None of them, as far as I found, does closed
  form catch-up of a frozen region; they either froze or ran a coarse sim continuously.
- X4 shows the risk of two rule sets: low-attention combat gives different outcomes than high attention, and the
  fixes went on for years. Whatever Veach does, the coarse rules must be the single source of truth for outcomes, and
  the full simulation must be a presentation of them, or both must agree by construction.
- Elite's aggregate, once-per-period tick is the closest precedent to what Veach needs at universe scale, and its
  re-run-from-earlier-values incident shows the value of an aggregate sim that is a pure function of stored inputs.

### Gaps
- No primary source on X4's low-attention rules for trade, mining or docking, or on its bubble radius.
- No primary source for NMS creature spawning (community only: creatures appear around the player and need open
  space; the fan wiki marks some fauna "non-simulated") — [fan wiki](https://nomanssky.fandom.com/wiki/Fauna),
  [Steam](https://steamcommunity.com/app/275850/discussions/0/3278065083967257005). Treated as unverified.
- Nothing found on Stellaris's daily/monthly tick structure (only community lag reports), Mount & Blade's tick rate,
  or GTA's ambient population algorithm (mod pages only).
- DF's off-site update frequency and battle resolution: not found.

## 2. Ecosystems: aggregates vs individuals, flora placement vs growth

### Takeaway
Shipped space games spawn creatures on demand around the player and do not simulate populations; the games that do
simulate populations (Eco) run a small world on a 24-hour server. Lotka-Volterra is easy to write but unstable in
games. Aggregate density per region with individuals materialized near observers is the only option that scales to
planets.

### Cited Findings
- Eco (Strange Loop): tracks "the population and life cycle of every plant and animal"; overharvesting and pollution
  can cause permanent extinction; pitched as thousands of simulated plants and animals on a server running 24 hours a
  day — [PC Gamer](https://pcgamer.com/eco-is-an-open-world-survival-game-that-will-let-you-destroy-the-planet),
  [Wikipedia](https://wikipedia.com/wiki/Eco_(2018_video_game))
- The Reef uses Lotka-Volterra as its core equations (dx/dt = bx − hxy, dy/dt = chxy − dy) and lets players watch
  "the ecosystem collapse or the population explode" — [itch.io](https://jakemchugh1.itch.io/the-reef);
  interactive explainer — [Red Blob Games](https://www.redblobgames.com/dynamics/predator-prey/); a Chalmers thesis
  compares a Unity agent ecosystem against LV cycles — [Chalmers](https://odr.chalmers.se/server/api/core/bitstreams/de3d3f4f-9b33-4b8e-80d9-71845b6d6d10/content)
- A student project reports LV "too unstable" for a game unless mathematical accuracy is the goal (search summary;
  source attribution unclear, unverified)
- NMS Origins reset creatures while keeping terrain, i.e. fauna is regenerated from seed, not stored —
  [NMS Origins](https://nomanssky.com/origins-update/)
- AC Unity / Hitman pool and recycle NPC identities near the player (see section 1)

### Inferences
- Classic LV is neutrally stable (closed orbits): any numeric error or perturbation drifts the cycle, and it has no
  closed-form solution, so catching up needs integration. A logistic model per species with a predation term that is
  damped toward a generated equilibrium has a closed form for the single-species case,
  N(t) = K / (1 + ((K − N₀)/N₀)·e^(−rt)), and relaxes toward equilibrium, so persisted deviations expire.
- Flora should be placement (a pure function of seed and position) plus persisted removals with regrowth timers;
  growth simulation is an extension.

### Gaps
- No measured cost of an aggregate ecosystem per region in any shipped game.

## 3. Factions, economy, traffic

### Takeaway
Two working models: X4 simulates every ship and station (cheaply when far), Elite simulates factions as numbers per
system on a daily tick. Star Citizen's agent model (Quanta) is designed but not shipped. Abstract records become
physical near players; none of the sources documents the materialization step.

### Cited Findings
- X4 every station and ship is part of the economy, even off-screen — [Steam](https://store.steampowered.com/app/392160/X4_Foundations/)
- X4 shortages resolve "by time in the simulation" (player), pirate fleets have little economy behind them (modder)
  — search summary, unverified
- Elite: influence per faction per system, daily aggregate tick, player actions batched into it — section 1 sources
- StarSim/Quanta: lightweight agents with jobs, no geometry or pathfinding; "contextually appropriate NPCs or events"
  are created for players from them — [Star Citizen Wiki](https://starcitizen.tools/Quantum_(game_system))
- EVE: NPC faucets and sinks are the levers on a player economy — section 1 sources
- DF: armies are abstract records that physically arrive — section 1 sources

### Inferences
- Traffic is easiest as scheduled voyages (origin, destination, departure tick, arrival tick, ship seed): position is
  a function of the clock, like an on-rails orbit, so off-screen traffic needs no simulation at all and a voyage can
  be materialized as a physical ship at the right place at any moment.
- Off-screen combat must be one aggregate rule seeded by the encounter, the same rule whether the player is far or
  never there; materialized combat near the player is the only place full rules apply (avoid X4's two-rule drift by
  never resolving the same encounter with both).

### Gaps
- No source on how X4 or Elite turns an abstract ship into a physical one near a player (spawn distance, state
  transfer).

## 4. Automated bases

### Takeaway
Factorio proves full per-tick simulation of production is memory-bound and only works because the whole map is one
game; Space Engineers throttles blocks by presence; idle games use closed forms with caps. A production graph with
constant rates between buffer events can be solved exactly, event to event.

### Cited Findings
- Factorio full simulation, memory-bound, sleeping entities — [FFF #204](https://factorio.com/blog/post/fff-204),
  [FFF #421](https://updater.factorio.com/blog/post/fff-421), [forum](https://forums.factorio.com/viewtopic.php?p=505768)
- Space Engineers tiered block updates by player/grid presence, physics only near players —
  [Marek Rosa 2020](https://blog.marekrosa.org/2020/08/space-engineers-server-optimizations/)
- Idle-game closed forms and their failure when rates change mid-period; caps on offline earnings —
  [Math of Idle Games](https://gamedeveloper.com/design/the-math-of-idle-games-part-i)

### Inferences
- Piecewise-linear event stepping: within a segment where no buffer is full or empty, every machine's rate is
  constant, so contents are linear in time; compute the earliest tick at which any buffer hits a bound, jump there,
  recompute rates, repeat. Cost is proportional to the number of state changes, not ticks. Factorio's "sleep when
  blocked" is the same idea per entity.
- This only works for machines whose behavior is declared as rates and capacities. Arbitrary per-tick script logic
  forces stepping.

### Gaps
- No source on how NMS or SE handle production when the owner is offline (SE servers keep simulating loaded grids;
  NMS base behavior unverified).

## 5. Weather and geology

### Takeaway
NMS shows storms and hazards as per-planet events; no precedent simulates planet-scale atmosphere physics in a game.
Weather and eruptions can be pure functions of (seed, position, tick), which removes both the catch-up and the sync
problem.

### Cited Findings
- NMS storms, tornadoes, firestorms; volcanoes on "disrupted core" planets — [NMS Origins](https://nomanssky.com/origins-update/)
- Space Engineers persists voxel edits — [Rosa](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/) (00-scope)

### Inferences
- Eruption schedule per volcano as a seeded point process on the clock (event k's time from hash(volcano id, k)),
  its terrain edit as a deterministic brush list from hash(volcano id, k); the edit enters the Q11 edit log when the
  region is next loaded (or live when observed), ordered by event tick with player edits.

### Gaps
- No precedent found for eruptions as persistent terrain edits in a shipped game.

## 6. Determinism and threading

### Takeaway
A counter-based (hash) random function keyed by domain, id and tick is the standard way to make results independent
of evaluation order, thread and whether a value is computed live or in catch-up. Veach's single PCG32 stream cannot
do that.

### Cited Findings
- Squirrel Eiserloh, "Math for Game Programmers: Noise-Based RNG", GDC 2017: position-based hash RNG (Squirrel3,
  later SquirrelNoise5 after a weakness was found in the talk's version) — [YouTube](https://www.youtube.com/watch?v=LWFzPP8ZbdU);
  community ports and header (unverified which gist carries SquirrelNoise5) — [gist](https://gist.github.com/kevinmoran/0198d8e9de0da7057abe8b8b34d50f86)
- Flecs `.rate(N)` counts pipeline runs; `.interval(s)` accumulates deltas — repo `docs/guides/flecs-v4/upstream/Systems.md`
- Float determinism rules (no libm transcendentals across ARM/x64) — [Fiedler](https://gafferongames.com/post/floating_point_determinism/) (00-scope)

### Inferences
- `.rate(N)` is deterministic under fixed steps but counts from system creation, not from the universal clock, so it
  does not survive save/load alignment; a scheduler keyed on `tick mod N` and staggered by hashed id does.
- `.interval()` accumulates float seconds: avoid for simulation.

### Gaps
- No measurement of Flecs multithreaded determinism; it depends on systems writing disjoint data, which is our rule
  to enforce.

## 7. Cost on M1-class vs M3 Pro-class CPUs

### Takeaway
M1: 4 performance + 4 efficiency cores. M3 Pro: 6P+6E (or 5P+6E), with P cores up to ~30% faster single-thread than
M1 Pro (Apple's claim). Without a job system Veach uses one core, so the difference is ~1.3x; with VCH-015 it is
roughly 2x or more for parallel aggregate work. Flecs iteration itself is ~0.5-1 ns per entity per component; the
cost is the per-agent logic.

### Cited Findings
- M3 Pro: 12-core (6P+6E) or 11-core (5P+6E); single-thread up to 30% faster than M1 Pro; M3 E cores up to 50%
  faster than M1's — [Apple newsroom](https://www.apple.com/newsroom/2023/10/apple-unveils-m3-m3-pro-and-m3-max-the-most-advanced-chips-for-a-personal-computer/);
  base M1 is 4P+4E — [Notebookcheck](https://www.notebookcheck.net/M1-vs-M3-vs-Apple-M3-Pro-12-Core_12937_15110_15115.247596.0.html)
- Independent testing: M3 Pro P cores consistently faster and more efficient than M1 Pro's — [Eclectic Light](https://eclecticlight.co/2023/12/12/evaluating-the-m3-pro-summary/)
- Flecs single-component iteration over 1M entities ~0.46 ms (≈0.46 ns/entity, 2018 MacBook Pro) —
  [ecs_benchmark](https://github.com/SanderMertens/ecs_benchmark); 2M entities through 7 small systems ≈37 ms on
  Linux/GCC 13 (≈18 ns/entity across systems) — [abeimler/ecs_benchmark](https://github.com/abeimler/ecs_benchmark)
- Hitman: 1,200 per zone, 500 on screen at 30 fps on 2012 consoles with tiered LOD (section 1)

### Inferences
- Budget estimates in the deliverable (live agents per tier, aggregate records per tick) are my estimates and must
  be measured on the M5 Max, then scaled by single-core ratios. Marked unverified there.

### Gaps
- No measured Luau per-iteration cost for our dispatcher; no M1 or M3 Pro machine to measure on.
