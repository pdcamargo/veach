# Q2 raw notes: celestial mechanics and time

Research notes for [../02-celestial-mechanics-time.md](../02-celestial-mechanics-time.md). Date: 2026-11-27.
Measurements were taken on the M5 Max with a throwaway benchmark in the session scratchpad (not in the repo), built
with `clang++ -O2 -std=c++20`, Apple libm. Derivations are marked "derived"; anything I could not confirm from a source
is marked "unverified".

## 1. On rails vs n-body vs hybrids: what precedents do

### Takeaway
Every shipped game with a persistent or shared world puts celestial bodies on rails (closed-form Keplerian), and only
ships or small objects get integrated. Full n-body exists in sandboxes (Universe Sandbox) and mods (Principia), where
accuracy loss at high time steps and chaotic sensitivity are accepted or fought with heavy numerics.

### Cited Findings
- KSP: on-rails (non-physical) time warp goes up to 100,000x; physical warp keeps physics running and offers only 1x,
  2x, 3x, 4x; on-rails warp can't be used while the craft is under forces other than gravity, e.g. in an atmosphere,
  unless landed and stopped — [KSP wiki, Time Warp](https://wiki.kerbalspaceprogram.com/wiki/Time_Warp) (via search
  snippet; the page itself refused automated fetches, so treat exact wording as unverified).
- KSP: a forum answer explains that rails warp "isn't actually simulating anything, it is just jumping ahead in time" —
  [KSP forum](https://forum.kerbalspaceprogram.com/topic/107368-is-there-a-reason-for-the-time-warp-limits-at-different-altitudes)
  (community, not developer).
- KSP stock altitude limits per warp level exist, but I could not get the stock table; the BetterTimeWarp mod's own
  limits (e.g. 100,000 m for 10,000x) are mod values, not stock — [spacedock](https://spacedock.info/mod/168/BetterTimeWarp).
- KSP API exposes a `TimeWarp` class with rate arrays —
  [KSP API](https://www.kerbalspaceprogram.com/ksp/api/class_time_warp.html) (not read in detail).
- Principia, a KSP mod, replaces patched conics with n-body and extended-body gravitation using symplectic
  integrators — [GitHub](https://github.com/mockingbirdnest/Principia),
  [KSP forum](https://forum.kerbalspaceprogram.com/topic/162200-wip181-191-1101-1110%E2%80%932-1122%E2%80%935-principia%E2%80%94releases-every-new-moon%E2%80%94n-body-and-extended-body-gravitation/page/17).
- Principia uses correctly rounded elementary functions (two correctly rounded cube-root implementations depending on
  FMA availability) and maintains a fork of CORE-MATH — [Principia IRC log, 2025-07-20](https://esper.catirclogs.org/principia/2025-07-20),
  [mockingbirdnest/core-math](https://github.com/mockingbirdnest/core-math).
- Universe Sandbox: n-body integration; turning up the time step lowers accuracy (a 22-day step computes Mercury only
  four times per orbit, "not enough accuracy to maintain a stable orbit"); it substeps and automatically caps
  simulation speed to keep accuracy — [Universe Sandbox blog](https://universesandbox.com/blog/?p=1704),
  [Sim Settings wiki](https://universesandbox.fandom.com/wiki/Sim_Settings_Menu). Integrator list (default PEFRL) from a
  community wiki — [Universe Sandbox wiki](https://universesandbox.gamepedia.com/N-Body_Simulation) (unverified, may be stale).
- Outer Wilds: a PC Gamer article (search snippet; the page did not render when fetched) says inverse-square gravity
  applies to only one mass, the sun, that there are spheres of influence for the sun and each planet, and that the game
  is "pure simulation" which "you can actually break ... because they're not on rails" —
  [PC Gamer](https://pcgamer.com/how-to-build-a-black-hole). Timestep and determinism details: not found. The
  22-minute loop is documented — [GDC 2020 news](https://gdconf.com/news/see-4d-level-design-outer-wilds-deconstructed-gdc-2020).
- Elite Dangerous: a Frontier forum reply says orbital characteristics are calculated once per system, after which
  everything is fixed except orbital position and rotation (i.e., on rails) —
  [Frontier forum](https://forums.frontier.co.uk/threads/are-the-orbits-of-the-planets-physically-correct.394502/post-6203566)
  (forum, unverified as official). Planets passing through each other when they "collide" — same thread family,
  [PC Gamer](https://www.pcgamer.com/the-mind-bending-science-behind-the-planets-of-elite-dangerous) (unverified).
- Stellar Forge forms each system "from first principles" —
  [80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/) (already cited in 00-scope).
- SpaceEngine: Keplerian orbits and very large time acceleration (no primary source found; unverified).

### Inferences
- On rails is the only option that gives O(1) evaluation at an arbitrary tick, zero bytes of state to sync or save,
  unlimited warp and no drift. That matches Veach's headless, MCP-first, record/replay design.
- Outer Wilds' selective-gravity simulation is a hand-tuned single-system design with a 22-minute lifetime; it does
  not transfer to a persistent, seeded, multi-player galaxy.
- Principia shows that if anyone ever wants n-body, they also need correctly rounded math; the cost lives in the math
  library and the integrator, not the game.

### Gaps
- No primary source for SpaceEngine's orbit model or Elite's rotation model. Elite's orrery internals are forum lore.
- Outer Wilds' timestep is unconfirmed.

## 2. Kepler solvers and cost

### Takeaway
Elliptic Kepler with Newton iteration and a fixed iteration count is cheap and robust: measured ~71-77 ns per body
for a full position evaluation on the M5 Max with Apple libm trig; 10,000 bodies is ~0.75 ms on one core. A fixed
count of 6 iterations converges to machine precision for e < 0.7, 12 for e ≤ 0.9999. Universal variables are for
ships and hyperbolic objects.

### Cited Findings
- JPL's "Approximate Positions of the Planets": six Keplerian elements plus linear rates per century, referenced to
  J2000 ecliptic; valid 1800-2050 (Table 1) or 3000 BC-3000 AD (Table 2a/2b); Mercury error ~15″ in longitude and
  ~1,000 km in distance; Kepler solved with the starter E₀ = M + e*·sin M and Newton steps until |ΔE| ≤ 10⁻⁶ degrees;
  then x′ = a(cos E − e), y′ = a√(1 − e²) sin E, rotated by ω, I, Ω —
  [JPL SSD](https://ssd.jpl.nasa.gov/planets/approx_pos.html).
- Wisdom & Hernandez (MIT), "A Fast and Accurate Universal Kepler Solver without Stumpff Series": universal-variable
  solver, performs better than methods based on the Stumpff series —
  [arXiv:1508.02699](https://arxiv.org/abs/1508.02699).
- Brandt et al. 2021 report ~60 ns solves only when vectorized over many epochs; slightly faster than Markley's
  algorithm, which is Octofitter's default — [Octofitter paper, arXiv:2402.01971](https://arxiv.org/pdf/2402.01971).
- Tommasini & Olivieri (ENRKE, ENP5KE) aim for high speed and optimal accuracy; a related 2020 paper reports a few
  ns per point when generating many points —
  [A&A 2022](https://www.aanda.org/articles/aa/full_html/2022/02/aa41423-21/aa41423-21.html),
  [Mathematics 2020](https://doaj.org/article/0d9c56ce47c544468222bce493c6ceaa).
- Philcox, Goodman & Slepian's "Kepler's Goat Herd" is about 3× faster than Newton (blog summary) —
  [John D. Cook](https://www.johndcook.com/blog/2021/04/01/efficient-kepler-equation/).
- Textbook references not fetched: Vallado, *Fundamentals of Astrodynamics and Applications* (universal-variable
  propagation, algorithm "KEPLER"); Curtis, *Orbital Mechanics for Engineering Students* (Algorithm 3.3 universal
  anomaly); Meeus, *Astronomical Algorithms*. (unverified page/algorithm numbers)

### Measured (M5 Max, Apple libm, -O2, scalar)
| Case | Result |
|---|---|
| Full eval: Newton 4 iters + full ω/I/Ω rotation with trig per call | 71.0 ns/body; 10k bodies 0.71 ms |
| Eval with cached 3×2 rotation, fixed 6 Newton iters | 76.7 ns/body; 10k bodies 0.77 ms |
| Starter E₀ = M + e sin M (π for e > 0.8), worst residual |E − e sin E − M| | e<0.3: 4.4e-16 after 3 iters; e<0.7: 1.5e-13 at 4, 4.4e-16 at 5; e<0.99: 1.5e-7 at 8, 4.4e-16 at 12; e<0.9999: 8.9e-5 at 8, 4.4e-16 at 12 |
| Phase from double seconds `fmod(n·t, 2π)` vs exact integer `tick mod P`, 90-min orbit at r = 6.7e6 m | 1 yr: 1.2e-5 m; 10 yr: 2.2e-5 m; 100 yr: 1.8 mm; 1000 yr: 1.2 cm |

### Inferences
- Cost is dominated by sin/cos; the M1-class P-core is slower than the M5 Max (unverified factor, plausibly
  1.5-2×), so 10k bodies would be ~1-1.5 ms on M1, but a system needs tens to hundreds per step: microseconds.
- Fixed iteration counts make cost constant (good for frame budgets and SIMD) and remove a data-dependent branch.
- Double seconds are numerically fine for centuries; integer phase is cheap insurance and makes resonances exact.

### Gaps
- No M1 or M3 Pro measurement possible (only the M5 Max exists). The M1 factor is an estimate.
- Did not benchmark a correctly rounded sin/cos (CORE-MATH or LLVM libc) against Apple libm.

## 3. The clock, warp, multiplayer

### Takeaway
The engine already counts fixed steps as an integer (`Time.fixed_step`, uint64) and derives `elapsed` as
`fixed_steps * delta`. A universe clock separate from the engine step counter is needed the moment warp exists.
Precedents for multiplayer warp are: none (Elite), subspaces (KSP mods), and server-slowed time (EVE's TiDi).

### Cited Findings
- Veach: `Time { uint64 frame; uint64 fixed_step; double delta; double elapsed; }`, elapsed computed as
  `static_cast<double>(fixed_steps_) * fixed_delta_seconds` — `engine/ecs/include/veach/ecs/components.hpp`,
  `engine/runtime/src/engine.cpp:459`. Windowed runs take at most 8 fixed steps per frame —
  [ADR 0018](../../adr/0018-engine-lifecycle-and-phases.md). `veach::Rng` is PCG — `engine/core/include/veach/core/rng.hpp`.
- Record/replay hashes every entity's Scriptable components each frame and records steps per frame; MCP edits are
  recorded — [record-replay spec](../../specs/record-replay/spec.md).
- KSP multiplayer mods (KMP, DMP, Luna) use "subspaces": each player is on their own timeline and can sync forward to
  a player in the future; interaction with a future player's ships needs a catch-up —
  [KerbalMultiPlayer README](https://github.com/TehGimp/KerbalMultiPlayer/blob/master/README.md),
  [DarkMultiPlayer README](https://github.com/planteater8/DarkMultiPlayer/blob/master/README.md),
  [LMP forum](https://forum.kerbalspaceprogram.com/topic/168271-110-luna-multiplayer-lmp-beta/page/6) (community).
- EVE Online Time Dilation slows the game clock when a node is overloaded, in small increments; the clock can drop to
  around 5-10% of real time in big fights — [CCP dev blog](https://eveonline.com/article/introducing-time-dilation-tidi),
  [follow-up](https://www.eveonline.com/article/time-dilation-hows-that-going); 10% floor per
  [EVE University wiki](https://wiki.eveuniversity.org/Time_Dilation) (the CCP blog says there is a hard limit but no
  number). TiDi does not affect EVE server time: industry and skill timers run at normal speed (wiki).

### Inferences (derived)
- int64 ticks at 60 Hz last 2^63/60 s ≈ 4.9 billion years. Luau numbers and JSON (JavaScript) represent integers
  exactly up to 2^53 ticks ≈ 4.76 million years, so a tick can be a plain number in Luau and JSON if the universe
  clock is clamped below 2^53. At 100,000× warp that is ~47 years of continuous max warp.
- Exact phase: store a period as p = round(P_seconds · 60 · 2^16) (units of 2^-16 tick). Then
  frac = ((Δtick mod p) · 2^16 mod p) / p is exact integer math plus one IEEE division, which is deterministic on
  every platform. Overflow needs p < 2^47, i.e. periods < ~74,000 years. Integer ratios between periods (resonances)
  stay exact forever.
- TiDi is a precedent that a tick clock decouples game time from wall time, which also covers a server that falls
  behind.

### Gaps
- No primary source for KSP's stock warp altitude table.

## 4. Rotation, day, seasons, sun direction

### Takeaway
Sidereal and solar day obey 1/P_sol = 1/P_sid − 1/P_orb (prograde; + for retrograde), verified against NASA values
for Venus and Mercury. KSP picks a round solar day (6 h) and derives the sidereal period. The sun direction must be
"star position minus observer position, rotated into the body-fixed frame", which gives seasons, eccentric-orbit
equation of time, tidal locking, moons and binaries without special cases.

### Cited Findings
- NASA fact sheet: Mercury rotation 1407.6 h, day 4222.6 h, orbit 88.0 d, obliquity 0.034°, e 0.206; Venus rotation
  −5832.5 h, day 2802.0 h, orbit 224.7 d, obliquity 177.4°; Earth 23.9 h, 24.0 h, 365.2 d, 23.4°; Moon 655.7 h,
  708.7 h, 27.3 d, 6.7°; Mars 24.6 h, 24.7 h, 687.0 d, 25.2°; Jupiter 9.9 h, 9.9 h, 4331 d, 3.1°. Surface gravity
  Earth 9.8, escape velocity 11.2 km/s, mean orbital velocity 29.8 km/s —
  [NASA NSSDC](https://nssdc.gsfc.nasa.gov/planetary/factsheet/).
- Kerbin: sidereal rotation 21,549.425 s, solar day 6 h, equatorial radius 600,000 m, sidereal orbital period
  9,203,545 s; a synchronous orbit must match the sidereal period, not the solar day —
  [KSP wiki Kerbin](https://wiki.kerbalspaceprogram.com/wiki/Kerbin),
  [Stationary orbit](https://wiki.kerbalspaceprogram.com/wiki/Stationary_orbit) (via search snippets);
  the fandom wiki lists 21,600 s as sidereal (conflict; the official wiki's value is the consistent one) —
  [fandom](https://kerbalspaceprogram.fandom.com/wiki/Kerbin).
- Veach today: `sun_direction(const Sky&)` uses a fixed 23.44° declination formula over a 365-day year,
  `hour_angle = (time_of_day − 12)·π/12`, latitude, and `north`, with +Y as up —
  `engine/render/src/atmosphere.cpp:97`; Sky fields per [ADR 0038](../../adr/0038-physical-sky.md).

### Inferences (derived)
- Venus check: 1/(1/243.02 + 1/224.7) = 116.75 d = 2,802 h (retrograde: +). Mercury: 1/(1/58.65 − 1/87.97) = 175.97 d
  = 4,223 h. Kerbin: 1/(1/21549.425 − 1/9203545) = 21,600.0 s. All match the sources.
- Subsolar latitude = asin(dot(ŝ, pole)) in the body frame; for a circular orbit this is asin(sin ε · sin λ), λ the
  sun's longitude from the equinox.
- Tidal locking with uniform rotation (P_sid = P_orb) on an eccentric orbit gives physical libration for free.

### Gaps
- Precession and nutation (Earth axial precession ~25,800 yr) not researched further: out of scope at play timescales.

## 5. Eclipses and ring shadows

### Takeaway
Geometry only: angular radii θ = asin(R/d) of star and occluder, angular separation, circle-overlap area gives the
visible fraction. Ring shadow: intersect the ray to the star with the ring plane and look up the ring's radial
opacity. No sources needed beyond textbook geometry; no game postmortem found.

### Inferences (derived)
- Angular sizes are invariant if each length group scales by one factor: moon radius and moon orbit by s_p, star
  radius and planet orbit by s_sys. So total vs annular eclipses survive separate s_p and s_sys as long as each
  group's members share a factor. (This refines 00-scope §5, which said separate factors change them.)
- Candidate occluders for an observer are its ancestors, siblings and their children in the orbit tree: tens, not
  thousands.

### Gaps
- No game-industry source on eclipse rendering.

## 6. Scale s: what follows from fixed surface gravity

### Takeaway (derived)
With surface gravity fixed, GM_game = s² GM_phys. Low-orbit speed, escape speed and all orbital periods around that
body scale with √s. If the star's GM stays physical while orbits shrink, moons end up outside their planet's sphere
of influence at compressed scales; the same s² rule must apply to stars.

### Derivations
- g = GM/R² fixed and R → sR ⇒ GM → s²GM. v_circ = √(GM/r) with r → sr ⇒ v ∝ √s; T = 2πr/v ∝ √s; v_esc = √(2gR) ∝ √s.
  Earth (NASA): v_esc 11.2 km/s → 3.54 (s = 0.1) → 1.12 km/s (s = 0.01). LEO ~7.8 km/s, ~92 min → ~2.5 km/s,
  ~29 min → ~0.78 km/s, ~9 min. Moon month 27.3 d → 8.6 d → 2.7 d.
- KSP sanity check: Kerbin 600 km ≈ 0.094 Earth radii; √0.094 · 11.2 = 3.4 km/s escape (KSP wiki lists ~3.4 km/s;
  unverified exact value).
- Planet orbit around a star, orbit scaled by s_sys:
  (A) star GM physical: T ∝ s_sys^1.5: year 365 d → 11.5 d (0.1) → 8.8 h (0.01).
  (B) star GM ∝ s_sys²: T ∝ √s_sys: 115 d (0.1) → 36.5 d (0.01).
  KSP lies between: Kerbin a ≈ 0.091 AU, Kerbol GM ≈ 0.0088 Sun ⇒ T = 0.29 yr = 106.5 d (matches the 9,203,545 s).
- SOI r = a(m/M)^0.4. Under (A), with planet GM ∝ s_p² and orbit a ∝ s_sys: SOI ∝ s_sys·s_p^0.8, moon distance ∝ s_p,
  so moon/SOI ∝ s_p^0.2/s_sys. Earth-Moon today: 384,400 / ~924,000 km ≈ 0.42 (unverified SOI figure). At
  s_p = s_sys = 0.01: 0.42 × 0.01^-0.8 ≈ 17: the Moon is far outside Earth's SOI. Under (B): moon/SOI ∝
  (s_p/s_sys)^0.2: unchanged at one s; ×1.58 for s_p = 10·s_sys.
- Day length does not depend on R. Bounded below by breakup, ω²R = g ⇒ P_min = 2π√(R/g) ∝ √s: Earth 84 min,
  s = 0.01: 8.4 min. Synchronous orbit radius in body radii ∝ s^(−1/3): 6.6 R → 14 R (0.1) → 31 R (0.01).
- Equatorial surface speed 2πR/P ∝ s at fixed day: 465 m/s Earth → 4.65 m/s at s = 0.01.

## 7. Determinism

### Takeaway
IEEE basic ops are reproducible; transcendentals and FMA contraction are not. Clang contracts within expressions by
default and arm64 always has FMA, while x86-64 baseline has no FMA and MSVC stopped contracting under /fp:precise in
VS2022 17.0. So our Mac arm64 build and Windows x64 build disagree by default on any `a*b+c`. Correctly rounded libms
(CORE-MATH, LLVM libc) return identical bits everywhere.

### Cited Findings
- Clang: `-ffp-model=precise` is the default and enables `-ffp-contract=on` (contraction within a statement) —
  [cfe-commits 2021](https://lists.llvm.org/pipermail/cfe-commits/Week-of-Mon-20211108/391791.html),
  [LLVM discourse](https://discourse.llvm.org/t/fp-contraction-fma-on-by-default/64975). On x86 without FMA in the
  subtarget, `llvm.fmuladd` lowers to separate multiply and add — [llvm issue 54927](https://github.com/llvm/llvm-project/issues/54927).
- MSVC: "contractions will not be generated by default under the /fp:precise mode on all platforms starting from
  VS2022 version 17.0"; `/fp:contract` re-enables them —
  [MSVC blog](https://devblogs.microsoft.com/cppblog/the-fpcontract-flag-and-changes-to-fp-modes-in-vs2022/).
- LLVM libc / MSVC (Sept 2026): correctly rounded results "return the exact same bits on x86-64, ARM64, Windows,
  Linux, across datacenters, across library updates"; proven worst-case bounds for all standard univariate double
  functions (sin, cos, tan, exp, log named); fast path for >99.99% of inputs with latency similar to fast libms;
  MSVC uses it with `/Zc:cmath` — [MSVC blog](https://devblogs.microsoft.com/cppblog/bringing-correctly-rounded-math-to-production-with-llvm-libc/).
  The article is inconsistent about whether MSVC already uses it at runtime or "soon"; atan2 coverage not stated.
- CORE-MATH aims for correctly rounded functions at least as fast as current libms —
  [CORE-MATH talk](https://members.loria.fr/PZimmermann/talks/core-math-fpbench2023.pdf),
  [repo mirror](https://github.com/mockingbirdnest/core-math). IEEE 754-2019 does not require correctly rounded
  transcendentals (per the MSVC post).
- Fiedler on FP determinism — [Gaffer on Games](https://gafferongames.com/post/floating_point_determinism/) (cited in 00-scope).

### Inferences
- The authoritative orbit path needs only +, −, ×, ÷, sqrt (all correctly rounded in IEEE) and sin/cos. Make sin/cos
  correctly rounded, compile the module with `-ffp-contract=off` (Clang) and `/fp:precise` without `/fp:contract`
  (MSVC), and the output is bitwise equal on arm64 and x64.

### Gaps
- No measurement of correctly rounded sin/cos speed on Apple silicon.
