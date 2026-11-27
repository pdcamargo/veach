# 02: Celestial mechanics and time

How bodies move, spin and tilt; how days, years, seasons, local time, calendars and eclipses follow from that; how
time warp works; and how all of it stays deterministic and free to sync at every creator scale. Respects the (a) items
for Q2 in [00-scope.md](00-scope.md). Raw notes, measurements and every source:
[notes/02-celestial-mechanics-time-notes.md](notes/02-celestial-mechanics-time-notes.md).

**Verdict in one paragraph.** Celestial bodies run on rails: a tree of barycenters and Keplerian orbits, evaluated as
pure functions of an integer universe tick, with exact integer phase math and correctly rounded sin/cos so the
result is bit-identical on arm64 and x64. Nothing about a body's motion is ever stored, sent or integrated; it is
regenerated from seed + generator version + tick. Rotation, local time, seasons and the sun's direction are queries
over that tree, which replaces the Earth astronomy in today's `Sky`. Gameplay GM is s² times physical for every body,
stars included, so surface gravity stays physical and every orbital speed and period follows √s; year length and day
length are per-universe creator settings on top. Time warp is "rails warp" (the tick jumps, stepped simulation hands
off to closed-form catch-up) in single player; multiplayer has no free warp, only a server-owned skip.

## 1. The problem in Veach's terms

What exists:

- **The clock.** `veach::Time` holds `frame` and `fixed_step` as `uint64` and derives `elapsed = fixed_step × 1/60`
  as a double (`engine/runtime/src/engine.cpp:459`). Simulation runs at a fixed 60 Hz, at most 8 steps per windowed
  frame ([ADR 0018](../adr/0018-engine-lifecycle-and-phases.md)). That is already an integer clock; there is no notion
  of a universe time separate from "steps this process has run", so no epoch, no warp, no saved time.
- **Determinism.** `veach::Random` (PCG) seeded from `--seed`; record/replay hashes every Scriptable component each
  frame and stops at the first divergence ([spec](../specs/record-replay/spec.md)). A replay on Windows of a Mac
  recording is the natural cross-platform test, so anything hashed must be bit-identical across ARM and x64.
- **The sun.** `sun_direction(const Sky&)` (`engine/render/src/atmosphere.cpp:97`) is Earth astronomy in float32:
  a 23.44° declination cosine over a 365-day year, an hour angle from `time_of_day`, a `latitude`, a `north` angle,
  and +Y as zenith. One sun, one planet, ground level ([ADR 0038](../adr/0038-physical-sky.md)).
- **Layering.** Space concepts live in `space/` and the engine never knows them
  ([ADR 0042](../adr/0042-space-toolbox-layering.md)); generic pieces move down into `engine/`.

What Q2 must deliver (from 00-scope, class a): an int64 tick clock with phase that does not degrade; an orbit tree of
barycenters (binary and multiple stars, S- and P-type planets, moons of moons); a Kepler solver that handles e → 1 and
hyperbolic paths; per-body sidereal and solar time, seasons, tidal locking; an occlusion query for eclipses; a time
warp policy; and the scale rules for periods, speeds and gravity. Plus the constraints the brief puts on every
question: deterministic, testable headless and in CI, inspectable over MCP, extensible from Luau and C++ modules, and
correct from s = 1 down to NMS-like compression.

The hard part is not orbital mechanics, which is textbook. It is three choices that are expensive to change later:
the clock's representation (it is in every save, recording and packet), whether bodies are ever integrated (if they
are, every client must agree bitwise forever), and which quantities stay physical when s changes.

## 2. Options, with precedents

### 2.1 How bodies move

| Option | What it is | Who does it |
|---|---|---|
| **A. On rails, Keplerian** | Each body has fixed orbital elements relative to its parent; position at time t is closed form (solve Kepler's equation). Bodies never perturb each other. | Elite Dangerous: orbits computed once per system, then only orbital position and rotation advance ([Frontier forum](https://forums.frontier.co.uk/threads/are-the-orbits-of-the-planets-physically-correct.394502/post-6203566), unverified as official). KSP: celestial bodies on rails, ships on rails during warp ([KSP wiki](https://wiki.kerbalspaceprogram.com/wiki/Time_Warp)). JPL's own approximate ephemeris is this model with slowly drifting elements ([JPL SSD](https://ssd.jpl.nasa.gov/planets/approx_pos.html)). SpaceEngine (unverified). |
| **B. On rails + patched conics for ships** | A on bodies; ships fly Keplerian arcs around one body at a time, switching at sphere-of-influence (SOI) boundaries; under thrust or drag, integrate. | KSP (patched conics; physics only near the active vessel). |
| **C. Full n-body** | Integrate every body under every other body's gravity each step. | Universe Sandbox: n-body with substeps; large steps destroy accuracy ("a 22-day step computes Mercury four times per orbit"), so it caps speed to keep accuracy ([blog](https://universesandbox.com/blog/?p=1704)). Principia mod for KSP: symplectic n-body, with correctly rounded math for reproducibility ([GitHub](https://github.com/mockingbirdnest/Principia), [IRC](https://esper.catirclogs.org/principia/2025-07-20)). |
| **D. Selective simulation** | Integrate bodies, but only some masses attract (e.g. only the sun), with hand-tuned spheres of influence. | Outer Wilds: inverse-square gravity from the sun only, SOIs per planet, "not on rails", breakable if you try ([PC Gamer](https://pcgamer.com/how-to-build-a-black-hole), via snippet). A single hand-made system with a 22-minute life ([GDC 2020](https://gdconf.com/news/see-4d-level-design-outer-wilds-deconstructed-gdc-2020)). |
| **E. Hybrid: rails for bodies, restricted n-body for ships** | A on bodies; ships integrate under the summed gravity of nearby on-rails bodies (test particles that don't pull back). | Principia's ship side is close to this; common in trajectory tools. |

### 2.2 Kepler solvers

- **Elliptic, Newton on E with a fixed iteration count.** Starter E₀ = M + e sin M (π for e > 0.8). Measured on the
  M5 Max: converges to 4.4e-16 rad residual in 5 iterations for e < 0.7, 12 for e ≤ 0.9999. JPL uses the same
  iteration with a 10⁻⁶° tolerance ([JPL SSD](https://ssd.jpl.nasa.gov/planets/approx_pos.html)).
- **Non-iterative or vectorized solvers** (Markley 1995; Brandt et al. 2021 at ~60 ns only when vectorized over many
  epochs; ENRKE; "Kepler's Goat Herd", ~3× Newton) ([Octofitter](https://arxiv.org/pdf/2402.01971),
  [A&A 2022](https://www.aanda.org/articles/aa/full_html/2022/02/aa41423-21/aa41423-21.html),
  [Cook](https://www.johndcook.com/blog/2021/04/01/efficient-kepler-equation/)). Faster, but our cost is already tiny.
- **Universal variables** (one formulation for elliptic, parabolic and hyperbolic, as an initial-value problem from a
  state vector): Vallado's KEPLER, Curtis Algorithm 3.3, Wisdom & Hernandez 2015 without Stumpff series
  ([arXiv:1508.02699](https://arxiv.org/abs/1508.02699)). The right tool for ships (state vector in, state vector out)
  and for hyperbolic visitors, not for bodies, which are defined by elements.

### 2.3 The clock

| Option | Precision | Notes |
|---|---|---|
| **double seconds** | Measured: phase error of a 90-minute orbit is 1.8 mm after 100 years, 1.2 cm after 1,000 years. | Fine numerically for centuries. Not exact, so resonances drift by ulps; JSON and Luau carry it natively. |
| **int64 ticks at the fixed step** | Exact. 2^63 ticks at 60 Hz ≈ 4.9 billion years. | Matches ADR 0018. Below 2^53 ticks (4.76 million years) a tick is also exact as a Luau number and a JSON number, which removes the "int64 as string" problem for time. |
| **int64 ticks + exact phase** | Exact phase from integer math (below). | Costs one modulo per orbit. Periods in integer ratios stay locked forever. |

**Exact phase** (derived): store each period as p = round(P_seconds × 60 × 2^16), an int64 in units of 2^-16 tick
(0.25 µs resolution). Then phase fraction = ((Δtick mod p) × 2^16 mod p) / p: integer operations plus one IEEE
division, identical on every platform. It needs p < 2^47, so periods below ~74,000 years; longer ones (wide binaries,
Oort-cloud comets) are frozen or use the double path.

### 2.4 Time warp

| Option | What it does | Precedent |
|---|---|---|
| **None** | Real time only. | Elite Dangerous (planets orbit and rotate in real time). |
| **Physics warp** | Run k fixed steps per frame (the engine already allows up to 8). Exactly deterministic; bounded by CPU. | KSP 1×-4× ([KSP wiki](https://wiki.kerbalspaceprogram.com/wiki/Time_Warp)). |
| **Rails warp** | The universe tick advances by W per step; stepped simulation is suspended and things on rails jump. Allowed only when nothing the player controls is under non-gravity forces. | KSP up to 100,000×, not in atmosphere unless landed and stopped; "isn't simulating anything, it is just jumping ahead" ([forum](https://forum.kerbalspaceprogram.com/topic/107368-is-there-a-reason-for-the-time-warp-limits-at-different-altitudes)). |
| **Subspaces** (multiplayer) | Each player has their own timeline and syncs forward to others. | KSP mods KMP, DMP, Luna ([KMP](https://github.com/TehGimp/KerbalMultiPlayer/blob/master/README.md), [DMP](https://github.com/planteater8/DarkMultiPlayer/blob/master/README.md)). |
| **Server-owned rate** | One clock; the server may slow it under load or skip it by consensus. | EVE Time Dilation: the game clock slows to ~5-10% in huge fights while server time (industry, skills) runs normally ([CCP](https://eveonline.com/article/introducing-time-dilation-tidi), [EVE Uni](https://wiki.eveuniversity.org/Time_Dilation)). |

### 2.5 Rotation, sun and local time

- **Earth formulas** (what we have): declination and hour angle from a calendar. Correct only for Earth, one sun,
  circular year.
- **Query of the system state** (recommended): sun direction = position(star) − position(observer), rotated into the
  observer body's body-fixed frame, then into local east-north-up at the observer's latitude and longitude. Seasons,
  eccentric orbits (equation of time), retrograde spin, tidal locking, moons (whose "sun" moves with the planet's
  year and the moon's month) and binary stars all fall out with no special cases.
- **Sidereal vs solar day**: 1/P_sol = 1/P_sid − 1/P_orb (prograde; + for retrograde). Checked against NASA: Venus
  sidereal −243.0 d and year 224.7 d give a 116.75 d (2,802 h) solar day; Mercury 58.65 d and 87.97 d give 4,223 h
  ([NASA NSSDC](https://nssdc.gsfc.nasa.gov/planetary/factsheet/)). KSP picks a round solar day (6 h exactly) and
  derives the sidereal period, 21,549.425 s, from it ([KSP wiki](https://wiki.kerbalspaceprogram.com/wiki/Kerbin)).
  That is the right precedent: creators think in solar days.

### 2.6 Eclipses and ring shadows

Geometry, no precedent needed: the star and each occluder have angular radii asin(R/d); the visible fraction of the
star's disk is one minus the overlap area of two circles over the star's area (limb darkening optional). Candidate
occluders for an observer are its ancestors, siblings and their children in the orbit tree: tens. A ring shadow is the
ray toward the star intersected with each ringed body's ring plane, looking up a 1D radial opacity profile.

## 3. Tradeoffs

| | A rails | B rails + patched conics | C n-body | D selective sim | E rails + restricted n-body ships |
|---|---|---|---|---|---|
| Complexity | Lowest | Low; SOI handoff is the subtle part | High (integrator, stability, collisions) | Medium, hand tuning | Medium |
| Cost | O(bodies needed) closed form, ~75 ns each (measured) | Same plus ships | O(N²) per substep; warp multiplies substeps | O(N) per step | Rails + O(ships × nearby bodies) |
| Determinism | Pure function; bitwise with correctly rounded sin/cos | Same; ship arcs pure between burns | Chaotic: one ulp of difference grows exponentially; bitwise lockstep or state sync forever | Chaotic | Bodies pure; ships integrated, server-authoritative |
| Sync and save | Zero bytes: seed + version + tick | Ship state vectors only | Every body's state, every save | Every body's state | Ship states only |
| Any-tick query (MCP, catch-up, map) | O(1) | O(1) per arc | Integrate from last state | Integrate | Bodies O(1) |
| Time warp | Unlimited | Unlimited on arcs | Limited by accuracy (Universe Sandbox caps it) | Limited | Unlimited for bodies |
| Scale range | Any s; periods are parameters | Any s | Any s, but generated systems must be stable under integration | Hand-made only | Any s |
| Realism | No perturbations, no resonance dynamics, no Lagrange stability | Same, SOI discontinuities | Real | Tuned fake | Real for ships near bodies |
| Testability | Golden ephemeris, cross-platform hash | Same | Hard (sensitivity) | Hard | Bodies easy, ships moderate |

**On rails wins every column that matters to Veach** (determinism, sync, saves, MCP, warp, scale range) and loses only
realism that players of a NMS-style game do not see: no perturbations, so the generator must space orbits (Hill
spacing, 00-scope §2) because nothing will correct them. Ship trajectories are Q10's call; B and E both sit on top of
A, so choosing A for bodies leaves Q10 free.

**Clock.** int64 ticks + exact phase costs nothing and removes a class of doubt (precision at year 300 of a server,
resonances, cross-platform `fmod` of a rounded double). Doubles would work for centuries; I'd still not want "works
for centuries" in a save format.

## 4. Proposed design

### 4.1 The universe clock

- `SpaceClock` singleton (toolbox): `tick` (int64, universe ticks since the universe epoch), `rate` (ticks per fixed
  step: 1 normally, W under rails warp, can be < 1 later for TiDi-style slowdown via fractional accumulation), `epoch`
  metadata. Reflected and Scriptable, so record/replay hashes it.
- The engine's `Time.fixed_step` keeps counting process steps; `SpaceClock.tick` is what celestial code reads. They
  differ as soon as a save is loaded or warp is used. The tick is clamped below 2^53 so Luau, JSON and MCP carry it
  as a plain number.
- Seconds for display or physics are `tick / 60.0`; never accumulate `delta` into a time.
- Whether the tick clock belongs in `engine/` is a "generic goes down" call: a monotonic, savable, warpable
  simulation clock is generic. Recommend the engine grow `Time.universe_tick` + rate later, if a second consumer
  appears; until then it lives in `space/`.

### 4.2 The orbit tree

- Nodes are entities: `OrbitNode { parent; kind; elements; period_q; gm_game; }`. Kinds are a registry of motion
  models, not an enum: `kepler_elliptic`, `binary_pair` (two children around their barycenter, one Kepler solve, split
  by mass ratio), `fixed` (root, rogue body), `hyperbolic` (visitors), and later `lagrange`, `spline`, `luau`.
- Elements: a, e, i, Ω, ω, M₀ at the epoch, plus the period as an int64 `period_q` (§2.3) that is **not** recomputed
  from a and GM: this is the on-rails decoupling seam. By default the generator sets it from the gameplay GM (§6).
- Planets around one star are S-type children of the star; circumbinary planets are P-type children of the binary
  node; moons of moons nest. Planet-moon pairs with large mass ratios (Pluto-Charon) are `binary_pair`; otherwise the
  moon orbits the planet's center (the planet's wobble is ignored).
- Evaluation: per fixed step, evaluate only the subtree relevant to the active system (tens to hundreds of nodes),
  parent before child, in double, each node in its parent's frame. Velocity comes analytically from the same E.
  Results feed Q1's frame tree; distant systems are evaluated on demand.
- Kepler: fixed iteration count (6 for generated planets with e < 0.7; 12 for comets up to e = 0.9999); hyperbolic
  via sinh Newton. Universal variables are for Q10's ships.

### 4.3 Rotation and local time

- `Spin { period_sid_q; phase0; pole; }`: pole as obliquity and its longitude relative to the orbit plane. Body-fixed
  orientation = pole rotation × R_z(2π × phase(tick)). Uniform spin with P_sid = P_orb gives tidal locking and correct
  libration on eccentric orbits; 3:2 locks are just ratios. Precession: a slot, off (00-scope: out at play timescales).
- Creators and generators specify the **solar day** for a body with a host star; the sidereal period is derived
  (KSP precedent). Tidally locked bodies have no solar day; for moons the toolbox reports the synodic day.
- Queries (C++, mirrored in `@veach` and MCP), all pure in (body, lat, lon, tick):
  `sun_direction_local`, `sun_elevation`, `local_solar_time`, `day_of_year`, `year_number`, `subsolar_latitude`,
  `season_phase`, `illuminance_top` (from **physical** distance and luminosity, never scaled; 00-scope §2),
  `eclipse_fraction`.
- The engine's `Sky` changes shape: it takes up to N sun directions in the camera's local frame, each with
  illuminance, angular radius and color, plus "up" and altitude from Q1. The Earth formula stays in the engine as a
  convenience for non-space projects; the toolbox writes the directions. Zenith stops being +Y (Q1/Q8).
- Calendars: a Luau formatter over `(body, tick)` → year, day, hour strings. Non-integer days per year are the
  formatter's problem (leap rules generated from continued fractions); the clock never knows about calendars.

### 4.4 Eclipses and ring shadows

- `occluders(observer_body, observer_pos, star, tick)` returns the bodies that can cover the star (ancestors,
  siblings and their children), each with angular radius and separation; `eclipse_fraction` combines them by
  circle-overlap. CPU, once per star per frame for the camera, and on demand for gameplay (solar power, temperature).
- Q8 consumes the same list as analytic sphere and ring occluders in shading (moon shadow on a planet, planet shadow on
  rings, ring shadow on a planet). The query is data; the render feature is a module.
- Angular sizes are invariant when each length group shares one factor: moon radius and moon orbit by s_p, star
  radius and planet orbit by s_sys. Total vs annular eclipses survive separate s_p and s_sys (this refines 00-scope §5).

### 4.5 Time warp

- **Physics warp**: 2×-4× by running more fixed steps per frame; deterministic, capped by the frame budget.
- **Rails warp**: `rate = W` (1,000× to 100,000×). Allowed only when every player-controlled body is landed and at
  rest, docked, or on a coasting arc (Q10). Stepped systems (physics, creatures, weather) must declare a catch-up
  (Q7) and are suspended; on exit, they run catch-up from the tick delta. Terrain streaming is unaffected because the
  player is in the body-fixed frame and the body's surface does not move in it.
- **Skip** (`set_tick(t)` with catch-up) is the primitive; rails warp is repeated skips. MCP uses it to jump to a
  solstice in tests.
- **Multiplayer**: one server-owned clock, no free warp. Optional consensus skip ("everyone sleeps"). Subspaces are
  rejected: they split one world into several timelines, which fights persistence, edits and interest management.
  A TiDi-style server slowdown is possible later because nothing reads wall-clock time.

### 4.6 Determinism contract (Q2's part of Q12)

- Authoritative celestial math uses only +, −, ×, ÷, sqrt (correctly rounded by IEEE) and sin/cos from a correctly
  rounded implementation (CORE-MATH or LLVM libc's, which promise identical bits on x86-64 and ARM64,
  [MSVC blog](https://devblogs.microsoft.com/cppblog/bringing-correctly-rounded-math-to-production-with-llvm-libc/)).
  No atan2/asin in the authoritative path (they are display-only).
- Compile it with `-ffp-contract=off` on Clang. The default `on` contracts `a*b+c` within a statement, and arm64
  always has FMA while the x86-64 baseline does not
  ([LLVM](https://lists.llvm.org/pipermail/cfe-commits/Week-of-Mon-20211108/391791.html),
  [issue 54927](https://github.com/llvm/llvm-project/issues/54927)); MSVC does not contract under `/fp:precise` since
  VS2022 17.0 ([MSVC](https://devblogs.microsoft.com/cppblog/the-fpcontract-flag-and-changes-to-fp-modes-in-vs2022/)).
  So today the Mac build and the Windows build disagree on any multiply-add.
- Fixed iteration counts; no data-dependent early exit. Luau calls the C++ query, never its own `math.sin`.
- CI: a test evaluates a reference system (binary star, P-type planet, moon of a moon, e = 0.9999 comet, hyperbolic
  visitor) at ticks {0, 1, 10⁶, 10⁹, 2^46, 2^53 − 1}, hashes the bit patterns and compares against a committed hash,
  on the macOS arm64 gate and on Windows x64 CI. A second test checks a "Sol" preset against JPL's approximate
  positions within JPL's stated error ([JPL SSD](https://ssd.jpl.nasa.gov/planets/approx_pos.html)).

## 5. Extension points

| What | Luau | C++ module | Toolbox core only |
|---|---|---|---|
| New system layouts, authored systems, overrides of generated elements | Data + generator hooks (Q3/Q5) | | |
| Calendars, time formatting, named days and seasons | Formatter over (body, tick) | | |
| Scheduled events on the clock (festivals, eruptions on a date, Outer Wilds-style loops) | Event scheduler, deterministic | | |
| New motion model (spline path, Lagrange halo, scripted station route) | Pure `f(tick) → pose` hook with `@veach` math, slower | Registered `MotionModel` with fixed-cost evaluation | |
| Integrated motion (n-body test particles, perturbed comets) | | Module, server-authoritative, outside the rails contract | |
| New occluder kinds (dust clouds, megastructures) | | Registered occluder | |
| Precession, nutation, variable day length | | Spin-model module | |
| The tick, exact phase math, tree evaluation order, the determinism contract | | | Core |

## 6. Scale: what follows from fixed surface gravity

All derived; numbers use NASA's Earth values.

**Gravity rule.** Surface gravity g = GM/R² stays physical. With R → sR, gameplay GM = s² × physical GM. This must
apply to **every** body, stars included. If the star keeps its physical GM while orbits shrink, a planet's sphere of
influence shrinks relative to its moons' orbits as s_p^0.2/s_sys: at s = 0.01, Earth's Moon would sit ~17× outside
Earth's SOI (today it sits at ~0.42 of it). With the uniform rule the ratio is (s_p/s_sys)^0.2: unchanged at one s,
×1.58 when s_p = 10 s_sys, which the generator's Hill-spacing check (Q3) must absorb.

**Consequences, all ∝ √s:**

| Quantity | s = 1 | s = 0.1 | s = 0.01 |
|---|---|---|---|
| Low orbit speed (Earth-like) | ~7.8 km/s | ~2.5 km/s | ~0.78 km/s |
| Low orbit period | ~92 min | ~29 min | ~9 min |
| Escape speed | 11.2 km/s | 3.5 km/s | 1.1 km/s |
| Moon's month (orbit ∝ s_p) | 27.3 d | 8.6 d | 2.7 d |
| Year, star GM ∝ s_sys² (recommended) | 365 d | 115 d | 36.5 d |
| Year, star GM physical (rejected) | 365 d | 11.5 d | 8.8 h |

KSP sits between the two year rows (Kerbin's 106.5-day year), and its 3.4 km/s escape speed matches the √s row for
Kerbin's 0.094 Earth radii.

**Day length is independent of R.** Limits that do depend on s: breakup period 2π√(R/g) ∝ √s (84 min at s = 1,
8.4 min at s = 0.01); synchronous orbit radius in body radii ∝ s^(−1/3) (6.6 R, 14 R, 31 R); equatorial speed ∝ s at a
fixed day (465 m/s → 4.65 m/s), which makes the body-fixed frame (Q1, Q9) easier at small s.

**Where on rails decouples.** Because `period_q` is stored, not derived, any orbit's period can differ from
2π√(a³/GM). That is invisible for orbits ships never share (a planet's orbit around its star, when interplanetary
travel is supercruise or FTL, Q10) and visible for orbits ships do share (a ship parked beside a moon drifts away if
the moon's period is not the GM-consistent one). So: **year length is a free creator setting; moon and low orbits stay
GM-consistent; day length is free.**

**Proposed per-quantity creator settings** (in the universe manifest, Q11):

| Setting | Default | Range / note |
|---|---|---|
| Surface gravity | physical (generator's value) | multiplier 0.25-4 |
| Gravity rule | GM_game = s² GM_phys for all bodies | not a setting; the toolbox's law |
| Year length | GM-consistent (√s_sys × physical) | multiplier, or "fixed N solar days" |
| Day length | physical rotation from the generator | global multiplier; per-body override; clamped above breakup |
| Moon orbit periods | GM-consistent | not exposed (ships share them) |
| Universe time rate (game s per real s) | 1 | single player only; multiplayer fixes it per server |
| Time warp | physics 4×, rails 100,000× in single player; none in multiplayer | per-game policy |

## 7. Interactions with the other questions

- **Q1 frames:** Q2 produces each body's pose (double position relative to its parent, orientation, velocity) per
  tick; Q1 composes them into frames. Bodies at s = 1 move ~500 m per tick (Earth's 29.8 km/s), so players must live
  in body-fixed frames, never the system frame. Under rails warp, bodies jump far each step: Q1 must handle a frame
  whose origin moves arbitrarily far between steps.
- **Q3 pipeline:** owns elements, masses, spin, obliquity, tidal-locking decisions and orbital spacing (Hill, Roche,
  S/P stability); Q2 only evaluates. Q3 must generate in physical units and apply the gravity rule and s at placement.
- **Q4 terrain:** none directly; rails warp never moves the ground in the body-fixed frame.
- **Q5 procgen graph:** Luau motion and calendar hooks need the deterministic math Q12 provides.
- **Q6 networking:** the clock is the only celestial state on the wire (a tick and a rate). The server owns it; clients
  extrapolate. The handshake carries seed, generator manifest and s. Conflict to resolve in the synthesis: if Q6 wants
  client-side prediction across a skip, the skip must be an event with a tick, not a state jump.
- **Q7 simulation LOD:** rails warp and skip require every stepped system to provide closed-form catch-up from a tick
  delta. This is Q7's founding constraint, set by Q2.
- **Q8 renderer:** consumes N sun directions with illuminance from physical distances, the occluder list for eclipses
  and ring shadows, and "up" per position. The `Sky` component changes shape (§4.3).
- **Q9 physics:** physics runs in the body-fixed frame of the nearest body; Q2 supplies the frame's angular velocity
  if Q9 ever enables fictitious forces (00-scope recommends off).
- **Q10 flight:** gameplay GM and SOI per body come from Q2's gravity rule; ships use patched conics (B) or restricted
  n-body (E) on top of rails. Warp eligibility (coasting vs thrusting) is Q10's state.
- **Q11 persistence:** saves store the tick, the rate and the scale settings; never body positions. Changing a body's
  elements in a generator update moves every base on it in absolute space but not in its body-fixed frame: edits must
  be body-fixed (00-scope already says so).
- **Q12 determinism:** §4.6 is Q2's slice of the contract; it needs Q12 to adopt correctly rounded sin/cos and
  `-ffp-contract=off` for authoritative code.

## 8. M1-class vs M3 Pro-class

- **CPU:** measured on the M5 Max, ~71-77 ns per body for a full Kepler evaluation with Apple libm trig (scalar, -O2);
  10,000 bodies ≈ 0.75 ms on one core. An M1-class P-core is slower (estimate 1.5-2×, unverified), so ~1.1-1.5 ms
  for 10,000. A system needs tens to hundreds of nodes per step: under 0.05 ms on either. Correctly rounded sin/cos
  may cost more per call (not measured); even at 2× it stays negligible. The difference between the two classes
  does not change the design.
- **GPU:** none needed for mechanics. Visual-only uses (orbit lines in a map, 100,000 belt debris instances evaluated
  in a compute pass) are cheap on both and must never feed gameplay (Q12).
- **Where the classes do differ:** in what Q2 hands to Q8. Each extra sun costs a sky-view LUT pass and a shadowed
  light; analytic eclipse and ring-shadow terms add per-pixel ALU. That budget belongs to Q8, but the product question
  "how many suns light a scene at once" shows up here first. Recommend: M1-class shows two shadowed suns, M3 Pro three
  or more (to be measured by Q8's spike).

## 9. Recommendation and what would change my mind

**Recommend:** bodies on rails (A), as a barycenter tree of registered motion models; int64 universe ticks with exact
integer phase; correctly rounded sin/cos and no FP contraction in the authoritative path; rotation, local time,
seasons and sun direction as pure queries of (body, lat, lon, tick); eclipses as an occluder query; gameplay GM = s²
physical GM for every body; year and day length as creator settings; physics and rails warp in single player, a
server-owned clock with no free warp in multiplayer. Ship motion (B or E) is Q10's decision on top of this.

**What would change my mind:**

- The owner wants emergent orbital dynamics as gameplay (slingshots that perturb moons, unstable systems, planet
  collisions): then C for a few bodies, inside a server-authoritative module, accepting state sync.
- Correctly rounded sin/cos turn out to cost far more than measured libm on Apple silicon: then our own polynomial
  sin/cos with fixed operations (still bit-identical, still fp-contract off).
- Q6 decides on no cross-platform play: bitwise equality would matter less, but replay tests still want it; I would
  keep it.
- Q10 chooses arcade flight with no orbits at all: the gravity rule then only sets surface gravity, and year length
  could be freely compressed; the rest stands.

## 10. What is expensive to undo

- **The clock representation** (int64 ticks at 60 Hz since a universe epoch): in every save, recording, packet and
  Luau script. Changing the tick rate later rescales every stored time.
- **On rails for bodies:** a persistent shared universe built on rails cannot switch to integrated bodies without
  storing and syncing every body's state and breaking every save's "where things are".
- **The gravity rule** (GM ∝ s²): every creator's tuning of flight, fuel and travel depends on it.
- **Period stored separately from GM:** if dropped later, every universe with a custom year length breaks.
- **The determinism contract:** once replays and servers rely on bit-identical orbits, loosening it is free, but
  tightening it after the fact means re-recording everything.

Cheap to change later: the Kepler solver, iteration counts, warp levels, calendars, the occluder query, and the
motion-model registry contents.

## 11. Proposed ADR titles

1. The universe clock: int64 ticks at the fixed step since a universe epoch, with exact integer orbital phase.
2. Celestial bodies move on rails: a barycenter tree of registered motion models evaluated as pure functions of the tick.
3. Gameplay gravity: every body's GM scales with s² so surface gravity stays physical; orbital speeds and periods follow √s. (Write after the owner's answer on year and day length.)
4. Rotation, local time and the sun are queries of the orbit tree; the `Sky` component takes sun directions instead of Earth astronomy.
5. Eclipses and ring shadows come from an analytic occluder query over the orbit tree.
6. Time warp: physics warp and rails warp in single player, a server-owned clock without free warp in multiplayer; catch-up is mandatory for stepped systems.
7. (With Q12) Authoritative celestial math: correctly rounded sin/cos, no FP contraction, fixed iteration counts, hashed across arm64 and x64 in CI.

## 12. Spike candidates

Each is a throwaway prototype in `space/` or a test, sized by scope.

1. **Orrery over MCP.** A seeded tree (binary star, P-type planet, planet with a moon that has a moon, e = 0.9999
   comet), evaluated per tick, drawn as debug orbit lines, positions exposed through an MCP query. Scope: one module,
   one debug render feature, one MCP tool. **Success:** a screenshot shows closed orbits with the comet's
   near-parabolic loop; a "Sol" preset matches JPL's approximate positions within its stated error; a Tracy zone shows
   the cost of 10,000 nodes per step.
2. **Sun from the system.** A tilted (23.4°), rotating planet; the `Sky` fed by the tree's sun direction for an
   observer at latitude 60°. Scope: the queries of §4.3 and a changed `Sky` input. **Success:** noon screenshots at
   the two solstices and an equinox show sun elevations of 53.4°, 6.6° and 30.0°; latitude 80° has polar night at the
   winter solstice; identical local times at s = 1 and s = 0.01 with the same config switch.
3. **Cross-platform orbit hash.** The §4.6 test, run once with platform libm and default contraction, once with
   correctly rounded sin/cos and `-ffp-contract=off`. Scope: a test and a CMake flag on one target. **Success:** the
   second configuration gives the same hash on the macOS gate and Windows CI; the first, if it differs, is the
   evidence for Q12.
4. **Eclipse and ring shadow.** A moon crossing the star as seen from the planet; a ringed planet. Scope: the occluder
   query plus a minimal shading term. **Success:** the illuminance curve over the eclipse (plotted from MCP queries)
   reaches zero at totality when the moon's angular size exceeds the star's and a plateau when it does not; a
   screenshot shows the ring's shadow band.
5. **Rails warp round trip.** A landed player and a coasting ship; warp 100,000× for a simulated year; exit. Scope:
   `SpaceClock.rate`, a catch-up interface, one test. **Success:** the ship's state after warp equals the analytic arc
   at the end tick; a recording replays with matching hashes; frame time stays flat during warp.

Spikes 1 and 2 are on the path to the first slice ("the correct time of day for where you are").

## 13. Product decisions surfaced

| Decision | Recommendation |
|---|---|
| Realism vs gameplay for year length | Default GM-consistent (√s_sys × physical, 36.5 days at s = 0.01); creators may compress further. Real 365-day years at s = 1 mean nobody sees a season change in a play session; say so in the preset. |
| Day length | Physical from the generator by default, with a universe-wide multiplier. A compressed preset should shorten days (NMS-style); the exact default is the owner's taste. |
| Time warp | Single player: physics 4× and rails up to 100,000×. Multiplayer: none, plus an optional consensus skip. No subspaces. |
| Universe time rate | 1:1 by default; a single-player setting only. |
| How many suns light one scene | Two on M1-class, three or more on M3 Pro-class, pending Q8's measurement. |
| Emergent orbital dynamics as gameplay | No for the toolbox core; possible later as a server-authoritative module. |
| Precession, nutation, light travel time, tides | Out (00-scope class c). |

## Sources

Games and tools:

- KSP wiki, Time Warp: https://wiki.kerbalspaceprogram.com/wiki/Time_Warp (via search; the page refused automated fetches)
- KSP wiki, Kerbin: https://wiki.kerbalspaceprogram.com/wiki/Kerbin; Stationary orbit: https://wiki.kerbalspaceprogram.com/wiki/Stationary_orbit; fandom (conflicting value): https://kerbalspaceprogram.fandom.com/wiki/Kerbin
- KSP forum on warp limits: https://forum.kerbalspaceprogram.com/topic/107368-is-there-a-reason-for-the-time-warp-limits-at-different-altitudes
- KSP API TimeWarp: https://www.kerbalspaceprogram.com/ksp/api/class_time_warp.html
- KSP multiplayer subspaces: https://github.com/TehGimp/KerbalMultiPlayer/blob/master/README.md, https://github.com/planteater8/DarkMultiPlayer/blob/master/README.md, https://forum.kerbalspaceprogram.com/topic/168271-110-luna-multiplayer-lmp-beta/page/6
- Principia: https://github.com/mockingbirdnest/Principia, https://esper.catirclogs.org/principia/2025-07-20, https://github.com/mockingbirdnest/core-math
- Universe Sandbox: https://universesandbox.com/blog/?p=1704, https://universesandbox.fandom.com/wiki/Sim_Settings_Menu, https://universesandbox.gamepedia.com/N-Body_Simulation (unverified)
- Outer Wilds: https://pcgamer.com/how-to-build-a-black-hole (snippet only), https://gdconf.com/news/see-4d-level-design-outer-wilds-deconstructed-gdc-2020
- Elite Dangerous: https://forums.frontier.co.uk/threads/are-the-orbits-of-the-planets-physically-correct.394502/post-6203566 (forum), https://80.lv/articles/generating-the-universe-in-elite-dangerous/
- EVE Online TiDi: https://eveonline.com/article/introducing-time-dilation-tidi, https://www.eveonline.com/article/time-dilation-hows-that-going, https://wiki.eveuniversity.org/Time_Dilation

Astronomy and astrodynamics:

- JPL, Approximate Positions of the Planets: https://ssd.jpl.nasa.gov/planets/approx_pos.html
- NASA NSSDC planetary fact sheet: https://nssdc.gsfc.nasa.gov/planetary/factsheet/
- Wisdom & Hernandez 2015, universal Kepler solver: https://arxiv.org/abs/1508.02699
- Octofitter (Brandt 2021 and Markley comparison): https://arxiv.org/pdf/2402.01971
- Tommasini & Olivieri, ENRKE: https://www.aanda.org/articles/aa/full_html/2022/02/aa41423-21/aa41423-21.html; https://doaj.org/article/0d9c56ce47c544468222bce493c6ceaa
- Kepler's Goat Herd summary: https://www.johndcook.com/blog/2021/04/01/efficient-kepler-equation/
- Not fetched (textbooks): Vallado, *Fundamentals of Astrodynamics and Applications*; Curtis, *Orbital Mechanics for Engineering Students*; Meeus, *Astronomical Algorithms*.

Determinism:

- LLVM libc correctly rounded math in MSVC: https://devblogs.microsoft.com/cppblog/bringing-correctly-rounded-math-to-production-with-llvm-libc/
- MSVC /fp:contract change in VS2022: https://devblogs.microsoft.com/cppblog/the-fpcontract-flag-and-changes-to-fp-modes-in-vs2022/
- Clang fp-contract default: https://lists.llvm.org/pipermail/cfe-commits/Week-of-Mon-20211108/391791.html, https://discourse.llvm.org/t/fp-contraction-fma-on-by-default/64975, https://github.com/llvm/llvm-project/issues/54927
- CORE-MATH: https://members.loria.fr/PZimmermann/talks/core-math-fpbench2023.pdf
- Fiedler, Floating Point Determinism: https://gafferongames.com/post/floating_point_determinism/

Veach: `engine/ecs/include/veach/ecs/components.hpp`, `engine/runtime/src/engine.cpp`, `engine/render/src/atmosphere.cpp`,
`engine/core/include/veach/core/rng.hpp`, [ADR 0018](../adr/0018-engine-lifecycle-and-phases.md),
[ADR 0038](../adr/0038-physical-sky.md), [ADR 0042](../adr/0042-space-toolbox-layering.md),
[record-replay spec](../specs/record-replay/spec.md), [00-scope.md](00-scope.md).
