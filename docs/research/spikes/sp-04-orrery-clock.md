# SP-04: orrery and universe clock (VCH-080)

Validates [ADR 0047](../../adr/0047-universe-clock.md) (int64 ticks, exact orbital phase),
[ADR 0048](../../adr/0048-bodies-on-rails.md) (bodies on rails, the sun from the system) and
[ADR 0049](../../adr/0049-scale-model.md) (periods scale with √s, elevations don't), with the determinism rules of
[ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md) and
[ADR 0067](../../adr/0067-dmath-on-llvm-libc.md). Research: [02-celestial-mechanics-time](../02-celestial-mechanics-time.md)
§12, spikes 1 to 3.

**Verdict.** The three decisions hold. Two formulas in the ADRs are wrong and need superseding text before the real
implementation (the phase formula overflows for any period over 1.13 years; the Kepler starter needs 16 steps, not
12, at e = 0.9999). Evaluation costs about 200 ns per body with dmath, about 3× the ADR's libm figure, which is still
negligible per system.

## What was built

The first toolbox code: `veach_space`, a static library in `space/` that links only `veach::veach`.

| File | What |
|---|---|
| `space/include/veach/space/clock.hpp`, `src/clock.cpp` | `UniverseClock {tick, rate}`, `period_q`, exact `phase_residue` and `phase` |
| `space/include/veach/space/orbit.hpp`, `src/orbit.cpp` | `System`, a parent-before-child tree of `OrbitNode`s; motion models registered by string id (`space:fixed`, `space:kepler_elliptic`, `space:kepler_secular`); the fixed-count Kepler solver; `Spin` and `orientation` |
| `space/include/veach/space/sun.hpp`, `src/sun.cpp` | `sun_local` (the star in east-north-up at a latitude and longitude, with elevation, azimuth, local solar time) and `subsolar_point` |
| `space/include/veach/space/presets.hpp`, `src/presets.cpp` | `sol(scale)`, `earthlike(scale)`, `reference_system()` |
| `space/include/veach/space/orrery.hpp`, `src/orrery.cpp` | `OrreryModule`: Scriptable `Orrery`, `UniverseClock`, `CelestialBody`, `SunObserver` |
| `tests/space_tests.cpp`, `tests/support/sp04_sol_reference.inl` | `[sp04.*]` unit tests, including the committed orbit hashes |
| `tests/bench/sp04_orrery_bench.cpp` | `veach-bench-sp04`: cost and the tables below |
| `apps/lab/src/sp04_spike.cpp`, `apps/lab/scripts/sp04_orrery.luau`, `sp04_sky.luau` | the lab imports the module; a script that logs Sol; the Sky lit from the system |
| `docs/research/spikes/sp-04/scripts/` | the Python references: JPL formulae, Horizons fetch, the `.inl` generator, solver convergence |

The system frame follows the engine's conventions: +Y is the reference plane's north (the ecliptic pole for Sol), +X
the J2000 equinox, so ecliptic (x, y, z) is (x, z, −y). Canonical files (`clock`, `orbit`, `presets`, and the direction
part of `sun`) use only dmath and + − × ÷ √, with fixed iteration counts. Sol's universe epoch is 1800-01-01 TDB, so the
whole 1800-2050 table interval has non-negative ticks and J2000 is tick 378,683,424,000.

## 1. The clock

| Check | Result |
|---|---|
| `phase_residue` against a 128-bit reference, 20,000 random (tick < 2^53, epoch, period_q up to 2^62) | equal in every case |
| A one-day period at every midnight and noon from 0 to 2^53 ticks; a quarter day before the epoch | exactly 0, 0.5, 0.75 |
| 2:1 (an Io-like 1.769 d) and 3:2 (Neptune-Pluto-like) at ticks 0, 1, 10^6, 10^9, 2^46, 2^53 − 1, 2^53/3, 2^53/7 | integer residues lock exactly; phases agree within 6.3e-17 (2:1) and 2.2e-16 (3:2) at every tick |
| Same 3:2 pair in double seconds (t / P), the rejected alternative | 1.8e-12 lock error and 1.9e-12 phase drift by 2^53 ticks |

**ADR 0047's formula overflows.** `((Δtick mod p) × 2^16) mod p` in int64 needs `p × 2^16 < 2^63`, i.e.
`period_q < 2^47`. A period_q of 2^47 is 2^31 ticks, which is **1.13 years**, not the ADR's 74,000 years. Mars, Jupiter
and every outer planet overflow. The 74,000 years is the bound for `period_q < 2^63`, which needs a 128-bit product or
the shift-and-subtract used here: 16 steps of `r = 2r; if r ≥ p: r −= p` on uint64, exact, portable, 6.9 ns per
phase including the division. The test covers periods up to 2^62 units and a 25,000-year comet orbit.

## 2. Bodies on rails

### Kepler solver

Measured on 200,000 random (e, M) pairs up to e = 0.9999 with M down to 10^-15, counting the Newton steps needed to
reach the double's conditioning limit ([kepler_conv2.py](sp-04/scripts/kepler_conv2.py)):

| Starter | e < 0.7 | e ≤ 0.8 | e = 0.99 | e = 0.9999 |
|---|---|---|---|---|
| ADR 0048: M + e sin M, π for e > 0.8 | 5 | 5 | 11 | **16** |
| M + e sin M everywhere | 5 | 5 | 9 | 23 |
| **Cubic for e > 0.8: min(π, ∛(6M))** | 5 | 5 | 5 | **6** |

So ADR 0048's "12 up to e = 0.9999" is not enough with its own starter (a unit test pins a 1e-6 rad miss at M = 10^-6).
The cubic starter (E³/6 ≈ M as e → 1) converges in 6 everywhere, so the solver uses one count, 6, for every e, on
|M| by symmetry. `[sp04.kepler]` checks the residual is within 4 ulp of |E| + |M| over 13 eccentricities × 416 anomalies.

### The Sol preset against JPL

Sources, pinned in `space/src/presets.cpp`:

- Planets: JPL SSD [Approximate Positions of the Planets](https://ssd.jpl.nasa.gov/planets/approx_pos.html)
  (E. M. Standish), Table 1, 1800-2050, J2000 ecliptic and equinox, copied digit for digit. The page HTML fetched on
  2026-11-27 has SHA-256 `9f7b30ca81cc548a879565bb3f70e6899a38ff5877cb1ff16bb04892f9c4d742`.
- The Moon: JPL SSD [Planetary Satellite Mean Elements](https://ssd.jpl.nasa.gov/sats/elem/), Earth's Moon, ecliptic,
  J2000 (a 384,400 km, e 0.0554, ω 318.15°, M 135.27°, i 5.16°, Ω 125.08°, P 27.322 d, ω period 5.997 yr, node period
  18.600 yr). It is split around the Earth-Moon barycenter by the DE430 mass ratio 81.30056.
- Reference positions: JPL Horizons, DE441, heliocentric (the Moon geocentric), ecliptic of J2000, TDB, at JD 2378496.5,
  2415020.5, 2433282.5, 2451545.0, 2460847.5 and 2469807.5 (1800, 1900, 1950, J2000, 2025-06-21 and 2050), fetched
  2026-11-27 through the Horizons API.

Each planet is a `space:kepler_secular` node: JPL's formula, with a, e, i, Ω and ϖ drifting linearly and the mean
longitude advancing by the exact clock phase. `[sp04.sol]` checks two things:

1. The C++ reproduces JPL's published formula, computed independently in Python, within 1e-9 relative at all 60
   (body, date) pairs.
2. Its error against DE441 stays within the limits below.

Worst error against DE441 over the six dates:

| Body | Longitude (") | Latitude (") | Distance (1000 km) | JPL's stated nominal error (", ", 1000 km) | Same with fixed elements (`kepler_elliptic`) |
|---|---|---|---|---|---|
| Mercury | 11.6 | 1.0 | 1.2 | 15, 1, 1 | 379, 71, 55 |
| Venus | 15.1 | 0.4 | 3.9 | 20, 1, 4 | 22, 31, 7 |
| EM barycenter | 8.2 | 1.6 | 7.1 | 20, 8, 6 | 76, 89, 12 |
| Mars | 58.8 | 1.1 | 19.1 | 40, 2, 25 | 267, 77, 339 |
| Jupiter | 309.1 | 7.1 | 549.4 | 400, 10, 600 | 309, 30, 638 |
| Saturn | 544.7 | 23.0 | 1874.2 | 600, 25, 1500 | 517, 69, 2749 |
| Uranus | 79.0 | 2.2 | 1341.4 | 50, 2, 1000 | 348, 16, 2432 |
| Neptune | 45.1 | 1.3 | 920.9 | 10, 1, 200 | 45, 2, 1038 |
| Earth | 8.2 | 1.5 | 7.1 | (none) | |
| Moon (geocentric) | 38,870 (10.8°) | 2,729 (0.76°) | 6.7 | (none: mean elements) | |

- **The published formula itself exceeds JPL's nominal errors** for Mars, Saturn's distance, Uranus and Neptune (up
  to 4.5×), against DE441. Ours equals the formula to 1e-9, so this is the approximation's accuracy, not the
  implementation's. JPL calls these "nominal" errors; they were fitted to an older ephemeris. The test's limits are the
  stated values, raised to 1.1× the formula's own measured error where that is larger.
- **Fixed elements are not enough for real-Sol fidelity**: freezing a, e, i, Ω and ϖ at J2000 (pure `kepler_elliptic`)
  misses by up to 25× the stated error across 1800-2050, mostly from perihelion drift. Generated systems don't care.
  A Sol preset that must match the sky needs the secular model, and registering it next to `kepler_elliptic` took one
  function, which shows the registry works as intended.
- **The Moon** is within 0.5° near J2000 and 2025, and drifts to 10.8° by 1800 because P is published to three
  decimals. That P is the sidereal month is an interpretation; the Horizons fit confirms it (the anomalistic reading
  misses by over 100°). Perturbations such as evection (1.3°) are out of scope.
- **The Earth's spin**, checked against the real sky:
  - J2000.0 from Paris (48.8566°N, 2.3522°E): the sun at 18.1° elevation, local solar time 12.102 h. Expected:
    declination −23.0° gives 18.1°, and 12:00 UT + 9.4 min of longitude − 3.3 min equation of time gives 12.10 h.
  - 2025-06-21 at Paris's noon: 64.58°, expected 90 − 48.86 + 23.44 = 64.58.
  - Same day at 00:00 TDB: −17.69° at 00:08 local solar time.
  - GMST at J2000 is applied to TDB rather than UT1, which is off by about 64 s (0.27°). Not verified beyond these
    points.

### Inspecting the orrery

Luau and MCP see four Scriptable components. Put an `Orrery { system = "sol" | "earthlike" | "reference", scale }` on an
entity. Each fixed step it advances its `UniverseClock` by `rate` (0 pauses, a spike extension of ADR 0047's rate ≥ 1),
writes a `CelestialBody` per node on named children (`orrery::earth`), and answers every `SunObserver { body, latitude,
longitude }`.

- **Lab script.** `veach-lab --headless --frames 3 --script scripts/sp04_orrery.luau` logs Sol at J2000. It is also a
  ctest (`lab: SP-04 orrery from Luau`, null RHI, so Windows CI runs it).
- **Over MCP** (scratch client): `set_component orrery UniverseClock {tick, rate: 0}`, `step_frames 1`, then
  `query_entities CelestialBody` and `get_component orrery::earth CelestialBody` or `paris SunObserver`. The numbers
  above came this way.
- **What doesn't work:** `eval` can't read a specific entity (Luau has no lookup outside systems) and can't call the C++
  query. So an agent asks "where is the Moon at tick t" with three tool calls and a paused clock. Module-registered MCP
  tools (VCH-100) or a `@veach` binding for the queries would make that one call. *Update:* VCH-100 added
  `orrery_state { system?, tick?, body?, latitude?, longitude? }`, which answers it in one call.

## 3. Cross-platform orbit hash

`[sp04.hash]` hashes every node's position and velocity, every spinning body's axes, and one observer's sun direction
and distance. FNV-1a over the bits, NaN canonicalized.

| Hash | Ticks | Value |
|---|---|---|
| Reference system: a binary star, a circumbinary planet, an S-type giant with a moon and a moon's moon (one tidally locked), comets at e = 0.95, 0.99 (25,000-year period) and 0.9999 | 0, 1, 10^6, 10^9, 2^46, 2^53 − 1, plus 1,000 spread over [0, 2^53) | `0xdaeaf79cdb3e0e48` |
| Sol, all 11 nodes and the Earth's spin, observer near Paris | 1,000 spread over 1800-2050 | `0x68f86b72db109716` |
| The Earth-like planet at s = 0.01 | 1,000 over two of its years | `0xbae95477aa9d6f98` |

| Build | Result |
|---|---|
| macOS arm64, Apple Clang 21, Debug (`-O0`) | all three equal |
| macOS arm64, Apple Clang 21, Release (`-O3`) | all three equal |
| Windows x64, MSVC, Debug | **pending**: CI runs `[sp04.hash]` once this branch is pushed (not pushed by the spike) |

`tools/scripts/wincheck.sh` type-checks every new file under clang-cl with the MSVC headers. It now also includes
`space/include`.

## 4. The sun from the system, at two scales

`earthlike(s)` is a planet on a circular orbit at 1 AU × s_o, radius 6,371 km × s_p, tilted 23.4°, with a 24-hour
solar day at every scale. GM = s² × physical, so the year is 365.257 days at s = 1 and 36.526 days at s = 0.01, a
ratio of exactly √s (within 1e-12). The sidereal period follows from 1/P_sid = 1/P_sol + 1/P_orb. The observer is at
60°N, standing under the sun's meridian at the instant of each event:

| Event | Noon elevation, s = 1 | Noon elevation, s = 0.01 | Ideal (90 − 60 ± 23.4) |
|---|---|---|---|
| June solstice | 53.398545111° | 53.398545111° | 53.4° |
| September equinox | 29.997886801° | 29.997886801° | 30.0° |
| December solstice | 6.597576075° | 6.597576075° | 6.6° |

- The two scales agree to 1e-9° (the unit test's limit; they print identical to 9 decimals).
- The 0.0015° to 0.0024° below ideal is real geometry: the observer's parallax, R/a = 4.3e-5 rad, which is the same at
  both scales because R and a scale together.
- Polar night at 80°N on the December solstice: the sun stays below −13.0° all day, at both scales (`[sp04.sun]`).

An observer at a fixed longitude sees the scales differ, by design. At s = 0.01 the year is 10× shorter per day, so the
season moves 10× faster between one noon and the next:

| Event | s | Noon at longitude 0 nearest the event | Its distance from the event |
|---|---|---|---|
| September equinox | 1 | 30.121° | 7.5 h |
| September equinox | 0.01 | 30.511° | 3.2 h |
| December solstice | 1 | 6.598° | 8.9 h |
| December solstice | 0.01 | 6.623° | 6.3 h |

Sunsets at 60°N, the sun's center at the horizon:

| Event | Local solar time, azimuth (s = 1) | Same, s = 0.01 |
|---|---|---|
| June | 21:14, 322.6° | 21:14, 322.4° |
| September | 17:59, 269.8° | 17:53, 268.1° |
| December | 14:46, 217.4° | 14:46, 217.4° |

### Screenshots

`sp04_sky.luau` lights the physical Sky from the observer. The lab spawns the orrery paused at the event from
`VEACH_SP04_SCALE`, `VEACH_SP04_EVENT` and `VEACH_SP04_TIME`. Today's `Sky` only takes Earth astronomy, so the script
maps the direction onto it: latitude 0 and its own equinox (declination 0), so the hour angle sets the elevation, and
`north` turns the azimuth. That is ADR 0048's case for `Sky` taking directions. The camera looks along the sun's
azimuth, 20° up, with a 90° field of view; the Flight Helmet stands 2 m toward the sun because the renderer draws
nothing without a mesh.

| | s = 1 | s = 0.01 |
|---|---|---|
| June noon (53.4°) | ![](sp-04/june_noon_s1.png) | ![](sp-04/june_noon_s0.01.png) |
| June sunset | ![](sp-04/june_sunset_s1.png) | ![](sp-04/june_sunset_s0.01.png) |
| September noon (30.0°) | ![](sp-04/september_noon_s1.png) | |
| December noon (6.6°) | ![](sp-04/december_noon_s1.png) | |
| December sunset | ![](sp-04/december_sunset_s1.png) | |

The sun disk sits where the geometry puts it: 33° above the image center at the June noon, 10° above it at the equinox,
just over the horizon in December. imgdiff, s = 1 against s = 0.01:

| Case | Result |
|---|---|
| Noon, all three events | mean FLIP 0.00000: the images are identical |
| June sunset | mean 0.0004, 36 pixels above 0.5 at the sun's edge |
| September and December sunsets | mean 0.00017 and 0.00007, pass |

The sunset differences come from the season moving faster at s = 0.01: the sunset falls at a slightly different point
of the year.

## 5. Cost

`build/release/tests/veach-bench-sp04` (`release` preset, `-O3`, strict FP flags), Apple M5 Max, best of 5:

| System | Nodes | ns per evaluation of all nodes | ns per node |
|---|---|---|---|
| Sol (`kepler_secular`; the Earth-Moon pair solves twice) | 11 | 2,520 | 229 |
| Reference (`kepler_elliptic`, e up to 0.9999) | 10 | 2,014 | 201 |
| Earth-like (`kepler_elliptic`, e = 0) | 2 | 223 | 112 |

| Piece | ns per call |
|---|---|
| `phase` (int64 mod, 16 shift-subtract steps, one division) | 6.9 |
| `solve_kepler` (e = 0.0167, 0.5, 0.9999) | 173, 174, 178 |
| `orientation` (spin) | 176 |
| `sun_local` (two node walks, orientation, local frame, asin, two atan2) | 497 |
| `dmath::sin`, for scale | 13.0 |

A Kepler body is about 200 ns, against the research's 71-77 ns with Apple libm. The difference is dmath: a solve makes
14 sin/cos calls at about 13 ns each, on the single no-FMA path. A system of 100 nodes costs 20 µs per fixed step, and
10,000 nodes 2 ms on one core. That is fine per system and not fine for 100,000 visual debris, which ADR 0043 keeps on
the GPU anyway. Cheap reductions, all still canonical:

- sin and cos of the same angle as one call: dmath has no `sincos`.
- Newton steps after the first two updating sin E and cos E by the angle-addition formula with a short polynomial in
  ΔE, which uses only + − ×.
- Precomputing the spin's constant obliquity and pole trig: `orientation` recomputes 4 of its 6 trig calls.
- One solve shared by the two children of a binary pair: the Earth and the Moon solve the same orbit twice today.

## Do the decisions hold?

- **ADR 0047, the clock: holds, with one correction.** Exact phase works, stays exact to 2^53 − 1, and resonances
  never drift, while double seconds drift by 1.8e-12. The phase formula must be specified with a 128-bit product or the
  shift-and-subtract loop, and the bound stated as `period_q < 2^63` (about 74,000 years). As written, `< 2^47` caps
  periods at 1.13 years.
  - Pausing (`rate = 0`) was useful for MCP inspection; the ADR allows only rate ≥ 1.
  - A universe epoch before the elements' epoch (Sol at 1800) works because each node carries its own `epoch_tick`.
- **ADR 0048, bodies on rails: holds, with three refinements.**
  - **The Kepler starter.** Use the cubic starter for e > 0.8 and 6 iterations for every e. The ADR's π starter needs
    16 at e = 0.9999.
  - **`binary_pair` as a barycentric split on the node, not a model.** A per-node mass fraction applied to any
    relative-motion model composes better than a separate model: the Earth-Moon pair needs the precessing secular
    model. ADR 0048's "one Kepler solve" then becomes the caching follow-up.
  - **The cost.** About 200 ns per body with dmath, 3× the figure the ADR quotes and still negligible per system, with
    the reductions above available.
  - **What was confirmed.** The sun from the system replaces Earth astronomy: it reproduces Paris's real sun, solstice
    and equinox elevations, polar night and the equation of time with no special cases. `Sky` taking directions is
    needed: the mapping hack only works for one sun.
- **ADR 0049, scale: holds.**
  - Elevations at the events are identical at s = 1 and 0.01. Periods scale by exactly √s and the day doesn't.
  - Product note for the owner's presets: at s = 0.01 the season moves 10× faster per day. A fixed place sees the
    equinox noon elevation move 4° per day instead of 0.4°, and sunset times drift faster day to day. This is the
    "year ∝ √s" default working as intended.

## Follow-ups

- **Supersede the two formulas.** A new ADR amending 0047 (the phase computation and its bound) and 0048 (the Kepler
  starter and iteration count, `binary_pair` as a mass fraction). Accepted ADRs are not edited.
- **Windows CI** must reproduce the three hashes when this lands; that is the remaining half of success criterion 3.
- **MCP and Luau access to the queries:** module-registered MCP tools (VCH-100, done: `orrery_state`) and a `@veach`
  binding for `sun_local`/positions at a tick, so agents don't need the paused-clock pattern.
- **The cost reductions above**, once a profile of a real system shows celestial evaluation (not before).
- **Hyperbolic orbits** (`space:kepler_hyperbolic`) were left out; the solver shape is the same with sinh/cosh, which
  dmath has.
- **Moon fidelity**, if a Sol preset should match the real Moon: a better mean motion (published P has 3 decimals)
  and the main periodic terms. Not needed for generated systems.
- **The `core.fp_nondeterministic` self-test** (ADR 0043) can now include one orbit evaluation.

## How it was measured

- Unit tests: `build/debug/tests/veach-tests -tc="*sp04*"` and the same in `build/release`.
- Cost and every table above: `build/release/tests/veach-bench-sp04`.
- Python references in [sp-04/scripts](sp-04/scripts/):
  - `fetch_horizons.py` queries the Horizons API for the six dates.
  - `jpl_ref.py` evaluates JPL Table 1 (secular and fixed) and compares it with Horizons.
  - `gen_inl.py` writes `tests/support/sp04_sol_reference.inl`.
  - `kepler_conv2.py` measures solver convergence.
- Screenshots: `VEACH_SP04_SCALE=1 VEACH_SP04_EVENT=june VEACH_SP04_TIME=noon build/debug/apps/lab/veach-lab --rhi metal
  --headless --frames 3 --script scripts/sp04_sky.luau --screenshot out.png`, then `veach imgdiff`.
