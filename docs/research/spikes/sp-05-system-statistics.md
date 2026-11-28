# SP-05: system statistics (VCH-081)

> **Since VCH-112** S1 and B1 run on `engine/procgen`'s stage runtime (`space/gen/src/stages.cpp`), with the same
> committed hashes. See the [procgen spec](../../specs/procgen/spec.md).

Validates [ADR 0044](../../adr/0044-counter-based-hash-randomness.md) (counter-based hash),
[0050](../../adr/0050-universe-addresses-and-seeds.md) (addresses and seeds) and
[0051](../../adr/0051-staged-generation-pipeline.md) (staged physical records that never read s) with spike S3.2 of
[research 03](../03-generation-pipeline.md#10-spike-candidates): 100,000 systems through S1 and B1, headless.

**Result: the three decisions hold.** All violation counts are zero. Stage hashes are equal on 1, 6 and 18 threads
and at s = 1, 0.1 and 0.01. S1 + B1 take 1.6 µs per system on one core. The spike found two problems:

- The hash composition needed a fix: without one, `hash(a, b) == hash(b, a)` and `hash(0, 0) == 0`.
- S1 needs planet radii. ADR 0049 has a gap for s_p > s_o.

## What was built

All of it is throwaway except possibly the hash header, which is where ADR 0065 puts it.

| Piece | Where | Notes |
|---|---|---|
| `veach::mix64`, `hash` (1-6 key words), `hash_id`, `unit_double`, `unit_float`, `range`, `HashStream` | `engine/core/include/veach/core/hash.hpp` | Header-only, constexpr, integer only |
| `SystemAddress`, `BodyAddress`, encode/decode, text form | `space/gen/include/veach/space/gen/address.hpp`, `src/address.cpp` | ADR 0050's field widths |
| Records, S1 `space:system`, B1 `space:body.physics`, stage hashes, validation | `space/gen/.../system.hpp`, `src/system.cpp` | Canonical rules of ADR 0043: `veach::dmath` only, `HashStream` only |
| Realization layer: the only code that takes s | `space/gen/.../realize.hpp`, `src/realize.cpp` | R × s_p, orbits × s_o or s_p, gameplay GM = g R² |
| `veach-bench-sp05-systems` | `tests/bench/sp05_system_stats.cpp` | Histograms (Markdown and CSV), violations, systems/s, hash checks, hash quality |
| Chart | `tests/bench/sp05_chart.py` → [sp-05/histograms.svg](sp-05/histograms.svg) | Standard library only |
| Unit tests `[sp05]` | `tests/space_gen_tests.cpp` | Run by the macOS gate and Windows CI |
| wincheck sees `space/*/include` | `tools/scripts/wincheck.sh` | One include root added |

`space/gen` is its own static library (`veach_space_gen`, linking `veach::core` only). `veach::space` links it, so a
parallel spike can turn `veach::space` into a library without touching it.

To reproduce:

```sh
build/release/tests/veach-bench-sp05-systems --systems 100000 --out build/sp05/csv_bias05
build/release/tests/veach-bench-sp05-systems --systems 100000 --bias 0 --out build/sp05/csv_bias0
python3 tests/bench/sp05_chart.py build/sp05/csv_bias05 build/sp05/csv_bias0 docs/research/spikes/sp-05/histograms.svg
```

### The hash

```text
h = mix64(seed ^ 0x6A09E667F3BCC908)           // SHA-512 IV[0]
for each key word w: h = mix64(h ^ mix64(w + 0x9E3779B97F4A7C15))
```

`mix64` is the SplitMix64 finalizer of ADR 0044. The first version computed `h = mix64(seed + golden)`, the same
transform the words get. The test vectors caught it: `hash(1, 2) == hash(2, 1)` and `hash(0, 0) == 0`. Salting the
seed differently fixes both. The composition is pinned by `static_assert`s against published vectors (FNV-1a 64,
SplitMix64's first output for seed 1234567) and by committed `CHECK`s in `tests/space_gen_tests.cpp`.

### The address layout

ADR 0050's field widths, in this order from the high bits down:

```text
[63..61] layer (3) | cell x (14 − L) | cell z (14 − L) | cell y (11 − L) | system index (13 + 3L) | body (9)
```

The layer sits at a fixed position at the top, so it decodes first and addresses sort by layer. The body index sits at
the bottom, so every body of a system shares the system's bits. The text form is `S:<16 hex>/B:<index>`. Every layer
round-trips at its maximum field values and rejects one past them. The bench used 100,000 distinct addresses spread
over all 8 layers.

### The model

This is direct sampling of calibrated distributions with the constraints enforced by construction. It is not a disc
simulation; research 03 names that as the fallback if formation is too slow. The values marked unverified below come
from standard references that were not fetched, as in research 03.

| Step | Formula and source |
|---|---|
| Star mass | Kroupa 2001 IMF: dN/dm ∝ m^-1.3 on 0.08-0.5 M☉, m^-2.3 on 0.5-100 M☉ (unverified) |
| Age, metallicity | Age uniform in 0.05-10 Gyr; [Fe/H] ~ N(-0.1, 0.25) (Nordström et al. 2004, unverified) |
| Main sequence | L: Duric 2004 piecewise (0.23 m^2.3, m^4, 1.4 m^3.5, 32000 m); R ∝ m^0.8 / m^0.57; T_eff by Stefan-Boltzmann; lifetime 10 Gyr m^-2.5; brightens 0.8 → 1.2 × L over the main sequence (unverified) |
| Evolved | Giant for 10 % of the lifetime; white dwarf: M_f = 0.109 M_i + 0.394 (Kalirai et al. 2008), Mestel cooling; neutron star from 8 M☉, black hole from 20 M☉, both without planets (unverified) |
| Habitable zone | Kopparapu et al. 2014, 1 M⊕: runaway greenhouse S = 1.107 + ..., maximum greenhouse 0.356 + ...; d = √(L/S) AU. The polynomial coefficients are recalled, not re-read from the paper (unverified) |
| Snow line | 2.7 AU √(L_ZAMS / L☉) (Hayashi 1981, unverified) |
| Disc | Inner edge 0.04 AU m^0.6, outer edge 35 AU m^0.5. Solids: 5 M⊕ inside and 40 M⊕ outside the snow line × m × 10^[Fe/H], lognormal scatter (disc mass ∝ M*, Andrews et al. 2013, unverified) |
| Planet masses | Rocky: lognormal, median √m M⊕. Beyond the snow line, per slot: gas giant with probability 0.07 m 10^(1.2 [Fe/H]) (Johnson et al. 2010, unverified), dN/dM ∝ M^-1.3 on 0.1-13 M_J (Cumming et al. 2008, unverified); ice giant 8-25 M⊕; otherwise an icy body |
| Spacing | Walk outward and inward from an anchor planet. Each neighbor sits K mutual Hill radii away, periapsis to apoapsis, with K = 11 + Exp(mean 7); one slot in five is doubled (a gap). Validation requires ≥ 10 (Chambers et al. 1996, unverified) |
| Radius | Rocky R = M^0.27 (Zeng et al. 2016), icy 1.2 × that; envelopes per Chen & Kipping 2017 (Neptunian M^0.589, Jovian M^-0.044) (unverified) |
| Moons | Giants: 0-6 regular moons of 10^-6 to 10^-4 of the planet (Canup & Ward 2006). Solid planets: one giant-impact moon in four, 0.1-2 % of the planet. Periapsis ≥ 1.5 × fluid Roche limit 2.44 R_p (ρ_p/ρ_m)^(1/3); apoapsis ≤ 0.49 (1 − 1.03 e_p) R_H (Domingos et al. 2006); moons 10 mutual Hill radii apart (unverified) |
| Spin | Tidal lock when the Gladman et al. 1996 despinning time is under the age (Q, k₂ textbook values, unverified) |
| B1 | g, v_esc; orbit-averaged instellation; T_eq = (S(1−A)/4σ)^¼; Jeans test v_esc ≥ 6 v₀ at an exobase of 4 T_eq, so μ_min = 72 R T / v_esc²; cosmic shoreline I/I⊕ < 0.43 (v_esc / 5.03 km/s)⁴, normalized through Mars (Zahnle & Catling 2017); SpaceEngine's temperature classes |
| Potentially habitable | In the HZ, rocky, 0.1-5 M⊕, inside the shoreline and keeping N₂ |
| Habitability bias (ADR 0066) | With probability `bias`, the anchor planet is drawn inside the HZ as a rocky 0.5-3 M⊕ planet; every other planet follows the physics. `bias` is a creator setting read from G0, not a scale |

Named sub-streams (`star`, `disk`, `planet_masses`, `orbits`; per body `spins`, `orbit_angles`, `moons`; B1
`albedo`) each have their own `HashStream` key, as ADR 0044 prescribes.

## Results

100,000 systems at universe seed 1 on the M5 Max, in a Release build. The figures are for bias 0.5 unless a row says
otherwise. All tables come from the bench's output.

![SP-05 histograms](sp-05/histograms.svg)

### Violations: all zero

| Check | Bias 0.5 | Bias 0 |
|---|---:|---:|
| Adjacent planets < 10 mutual Hill radii (periapsis to apoapsis) | 0 | 0 |
| Planet periapsis inside the star's Roche limit or 2 stellar radii | 0 | 0 |
| Moon periapsis inside the planet's fluid Roche limit | 0 | 0 |
| Moon apoapsis beyond 0.49 (1 − 1.03 e) Hill radii | 0 | 0 |
| Adjacent moons < 10 mutual Hill radii | 0 | 0 |
| Systems with more than 255 generated bodies (max seen: 37) | 0 | 0 |
| Non-finite record fields | 0 | 0 |
| Addresses failing encode/decode/text round trip | 0 | 0 |
| Realized moons outside Roche..Hill, s_p = s_o ∈ {1, 0.1, 0.01} | 0 | 0 |

The first run found 76 Hill violations in 10,000 systems. A walk step whose position failed was clamped to the snow
line instead of stopping. That was a generator bug, and the validation caught it.

### Throughput

| Run | µs/system | Systems/s |
|---|---:|---:|
| S1 only, 1 thread | 1.33 | 752,000 |
| B1 only (5 planets + 2 moons on average), 1 thread | 0.25 | 4,070,000 |
| S1 + B1, 1 thread | 1.58 | 635,000 |
| S1 + B1 + stage hashes, 6 threads | 0.67 | 1,490,000 |
| S1 + B1 + stage hashes, 18 threads | 0.34 | 2,940,000 |
| S1 + B1, 1 thread, Debug build | 30 | 34,000 |

ADR 0051 reopens if S1 + B1 can't hide behind an FTL transition on an M1-class CPU. At 1.6 µs per system that is
several orders of magnitude away, so B2-B4's coarse fields set the arrival budget, not S1. Scaling across threads is
sublinear (2.4× on 6 threads). The likely cause is the per-system heap allocations; it was not investigated.

### Hashes

| Check | Value |
|---|---|
| Unit test sample (seed 42, bias 0.5, 2,000 systems), S1 | `0xc28a0456eedc0301` |
| Unit test sample, B1 | `0xf2f6a4c18dbe873d` |
| Bench, 100,000 systems, 1 thread forward = 6 threads = 18 threads, interleaved backward (bias 0.5) | `0x048c5309874e64a2` |
| Same, bias 0 | `0x32894ccd65034153` |
| Records after realizing at s = 1, 0.1 and 0.01 (bias 0.5) | `0xdaca285d98ab30c2` at all three |
| Realized geometry at s = 1 / 0.1 / 0.01 | `0xf66abea9fe05b146` / `0xecdb840551ede736` / `0x018e317b986beec4` |

The stage API takes no scale: `StageContext` has no scale field. The bench and the unit test realize each system at
three scales, then hash its records. The record hashes are equal and the realized geometry differs. The unit test
also checks that realized radii scale by s and that surface gravity does not.

The committed values are verified on macOS arm64 (Debug and Release give the same hashes). `tools/scripts/wincheck.sh`
type-checks the new files for MSVC. **Windows x64 equality is not proven until CI runs `[sp05]`** after the branch is
pushed.

### Hash quality (ADR 0044's reopen condition)

| Test | Result | Ideal |
|---|---:|---:|
| Pearson r of unit_double over keys (.., i) and (.., i+1), 10^6 pairs | +0.0007 | 0, noise ±0.003 at 3σ |
| Avalanche of the index word: output bits flipped per input bit | 32.005 | 32 |
| Worst per-output-bit flip bias | 0.0009 | 0, noise 0.0013 at 3σ |

The histograms show no artifacts. A PractRand run over `HashStream` is still a follow-up.

### Histograms

Stars:

| Class | O | B | A | F | G | K | M | D (white dwarf) | NS | BH |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % of systems | 0 | 0.14 | 0.61 | 1.88 | 3.10 | 13.4 | 74.8 | 5.43 | 0.45 | 0.18 |
| Planets per system | — | 8.8 | 8.8 | 8.4 | 8.0 | 7.0 | 4.5 | 4.0 | 0 | 0 |
| % with a gas giant | — | 21.5 | 19.8 | 17.0 | 12.3 | 8.1 | 1.1 | 28.8 | 0 | 0 |
| % with an HZ planet, bias 0.5 / 0 | — | 72 / 42 | 74 / 47 | 74 / 49 | 75 / 52 | 77 / 54 | 77 / 55 | 0 | 0 | 0 |
| % with a potentially habitable body, bias 0.5 / 0 | — | 68 / 37 | 73 / 44 | 73 / 46 | 74 / 49 | 75 / 51 | 74 / 47 | 0 | 0 | 0 |

Giants are 0.32 % of stars. There are no O stars, because none younger than their 10 Myr lifetime were drawn.

Planets: 500,848 in all, 5.01 per system; 0.39 moons per planet.

| Composition | Rocky | Icy | Ice giant | Gas giant |
|---|---:|---:|---:|---:|
| % of planets | 46.4 | 46.5 | 6.2 | 0.9 |

| Planets per system | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10-14 | 15-24 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % | 0.6 | 1.4 | 17.7 | 18.0 | 15.4 | 12.2 | 9.5 | 7.3 | 5.4 | 3.9 | 7.6 | 1.0 |

| Mass (M⊕) | < 0.03 | 0.03-0.1 | 0.1-0.3 | 0.3-1 | 1-3 | 3-10 | 10-30 | 30-100 | 100-300 | 300-1000 | ≥ 1000 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % | 0.5 | 3.7 | 14.4 | 33.9 | 27.9 | 11.9 | 6.8 | 0.5 | 0.3 | 0.1 | 0.05 |

| Radius (R⊕) | < 0.5 | 0.5-0.71 | 0.71-1 | 1-1.41 | 1.41-2 | 2-2.83 | 2.83-4 | 4-5.66 | 5.66-11.3 | 11.3-16 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % | 2.5 | 11.9 | 28.6 | 31.5 | 14.4 | 4.3 | 3.1 | 2.9 | 0.4 | 0.5 |

| Period (days) | < 3 | 3-10 | 10-30 | 30-100 | 100-300 | 300-1000 | 1000-3000 | 3000-10⁴ | 10⁴-10⁵ | ≥ 10⁵ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % | 2.1 | 7.9 | 14.5 | 19.4 | 14.5 | 12.3 | 8.7 | 8.1 | 10.2 | 2.2 |

| Adjacent period ratio | < 1.4 | 1.4-1.6 | 1.6-1.8 | 1.8-2 | 2-2.2 | 2.2-2.4 | 2.4-2.6 | 2.6-2.8 | 2.8-3 | ≥ 3 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % | 4.4 | 16.0 | 14.4 | 11.0 | 8.6 | 6.6 | 5.0 | 3.9 | 3.1 | 27.1 |

| Mutual Hill spacing | 10-15 | 15-20 | 20-25 | 25-30 | 30-35 | 35-40 | 40-50 | 50-60 | ≥ 60 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % | 31.7 | 21.9 | 15.0 | 10.3 | 6.2 | 4.0 | 4.5 | 2.4 | 4.0 |

| T_eq (K) | < 50 | 50-100 | 100-150 | 150-200 | 200-250 | 250-300 | 300-350 | 350-400 | 400-500 | 500-600 | ≥ 600 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| % | 17.4 | 18.3 | 17.9 | 15.4 | 12.3 | 5.4 | 3.6 | 2.6 | 3.3 | 1.6 | 2.2 |

| HZ planets per system | 0 | 1 | 2 | 3 | 4+ |
|---|---:|---:|---:|---:|---:|
| % at bias 0.5 | 27.6 | 49.8 | 20.4 | 2.2 | 0.02 |
| % at bias 0 | 48.9 | 30.9 | 18.0 | 2.2 | 0.02 |

| Potentially habitable bodies per system | 0 | 1 | 2 | 3+ |
|---|---:|---:|---:|---:|
| % at bias 0.5 | 30.7 | 51.5 | 16.4 | 1.4 |
| % at bias 0 | 55.0 | 30.8 | 13.0 | 1.2 |

Other figures: 85 % of HZ planets are tidally locked (most orbit M dwarfs). 73 % of rocky planets keep an
atmosphere (66 % at bias 0).

### Against known exoplanet populations (from memory, unverified)

- **Shapes that match.** Gas-giant frequency rises with stellar mass: M 1 %, K 8 %, G 12 %, A 20 %. That is the trend of
  Johnson et al. 2010. Our absolute values count giants out to 35 AU, so they run above RV surveys' few-AU numbers. The
  period ratios peak at 1.4-1.8 with a long tail, the shape of Kepler multis (Fabrycky et al. 2014).
- **η⊕ at bias 0.** About half of FGK systems have a potentially habitable planet. Bryson et al. 2021 give 0.37-0.60
  for the conservative HZ, so physics alone already makes habitable planets common by the generous definition used
  here.
- **No sub-Neptunes.** Rocky planets are bare and ice giants start at 8 M⊕, so only 7 % of planets fall in 2-4 R⊕.
  That size is Kepler's most common, and the radius valley (Fulton et al. 2017) is absent.
- **No hot Jupiters.** There is no migration.
- **Too many outer bodies around M dwarfs.** About 47 % of planets are icy, a mix of super-Ganymedes and dwarf-planet
  analogues. A belt kind would absorb some of them.

## Do the decisions hold?

- **ADR 0044: holds.** `hash`, `hash_id`, `HashStream` and the 53-bit conversion work as specified. There is no
  correlation between neighboring keys, and the result is order- and thread-independent.
  - The composition needed the seed salt above. The ADR's description ("each key word absorbed through the SplitMix64
    finalizer chained with the golden-ratio increment") allows the symmetric version, so the test vectors must fix it.
  - The vectors live in `tests/space_gen_tests.cpp`, not yet in the `tests/golden/determinism/hash.json` the ADR names.
- **ADR 0050: holds.** The layout fits at every layer, and the most bodies in a system is 37 of 255.
  - The bulge-density reopen condition can't be measured without G1/G2 (SP-04's neighbor, S3.1). An estimate: 8,192
    systems in a 10 ly cell is about 280 stars per pc³. That is reached in the nuclear bulge (inner ~100 pc), not the
    main bulge, and layer 0 holds only the dimmest stars. Check it when G2 exists.
- **ADR 0051: holds, with one change and one note.**
  - **Radius and density belong in S1.** Moon Roche limits and the star-planet limit need them, so S1 computes radius
    from mass and composition. B1 keeps g, v_esc, T_eq, retention and classification. The stage table should move
    "radius" from B1 to S1.
  - **Speed and determinism hold.** No stage reads s, and S1 + B1 cost 1.6 µs. The canonical doubles are verified on
    macOS; Windows CI must confirm them.
  - **Note:** creator settings that change content (habitability bias) are generation inputs read from G0. They belong
    in the manifest fingerprint, unlike s.
- **ADR 0049: gap for s_p > s_o.** At s_p = s_o every realized moon stays inside its realized Hill limit, at all three
  scales. With separate factors, the moon-to-Hill ratio moves by (s_p/s_o)^(1/3), not the ^0.2 the ADR quotes.
  - At s_p = 0.1, s_o = 0.01, 21,443 of 195,436 moons (11 %) end up beyond the realized Hill limit.
  - At s_p = 0.01, s_o = 0.1 there are none.
  - "The generator's Hill-spacing check absorbs it" is therefore true only for s_p ≤ s_o.

## Decisions for the dev lead

1. **s_p > s_o.** Choose one:
   - (a) Allow only s_p ≤ s_o in the advanced setting. This is the cheapest option.
   - (b) Have the realization layer clamp moon orbits to the realized Hill limit. This changes moon periods only.
   - (c) Generate moons inside 0.49 (s_o/s_p)^(1/3) R_H. That makes records depend on scale, which ADR 0051 forbids.

   Recommendation: (a) now, (b) if a creator asks.
2. **Default habitability bias.** At 0.5, 69 % of systems have a potentially habitable body; at 0, 45 %. Physics
   alone is already within published η⊕. ADR 0066 asks for "common", and both values deliver it. Recommendation: keep
   the knob and default it to 0.25. Decide after S3.3 shows what these worlds look like.
3. **Where the hash vectors live.** Inline `CHECK`s or `tests/golden/determinism/hash.json` (ADR 0044). Recommendation:
   create the JSON with VCH-093 or the first non-spike user of `hash.hpp`. Keep the inline vectors until then.

## Follow-ups

- Windows CI run of `[sp05]` (committed S1/B1 hashes on MSVC x64), and a Release run on Windows.
- PractRand over `HashStream` streams with neighboring keys (ADR 0044's remaining reopen check).
- Model gaps to close when S1 becomes real code:
  - binaries and S/P-type stability (Holman & Wiegert 1999);
  - sub-Neptune envelopes and the radius valley;
  - migration (hot Jupiters);
  - belts and rings;
  - per-gas retention as a B1 hook;
  - Kopparapu coefficients re-read from the paper.
- Records are plain structs. Reflection needs array support first (VCH-099); kinds are stable string ids already.
- The canonical-file lint for banned calls (ADR 0043's fix list). `space/gen/src/system.cpp` carries the
  `// veach: canonical` header it will look for.
- Thread scaling: profile the allocations if S1 ever matters for throughput. It doesn't at 1.6 µs.
