# SP-09: planet octree and a governed descent, at s = 0.01, s = 1 and onto a 1 km asteroid (VCH-085)

Validates the streaming half of [ADR 0053](../../adr/0053-terrain-density-field.md) (a density field per body, a
body-local octree of 32³-cell chunks, 0.5 m LOD0 cells, marching cubes + Transvoxel, "never a hole") with the
amendments of [ADR 0074](../../adr/0074-amendments-from-sp-08.md), against the travel governor of
[ADR 0059](../../adr/0059-gravity-flight-travel.md) (|v| ≤ max(v_floor, d/τ), τ = 5 s, v_floor = 100 m/s) and the
scale model of [ADR 0049](../../adr/0049-scale-model.md) (radius × s, planetary relief × s^0.5, detail band in meters).
Research: [04-terrain](../04-terrain.md) spike S4.2, [10-gravity-flight-travel](../10-gravity-flight-travel.md) spike
S10-1. All numbers: Apple M5 Max, release preset, 2026-11-27.

**Verdict.**

1. **No holes, no cracks.** Every checkpoint of every run (orbit, 100 km, 10 km, 1 km, 100 m, after a 100 m skim, at
   10 m; s = 1, s = 0.01 with three relief exponents, the asteroid) welds to **0 open edges** over the whole drawn
   octree (up to 5.3 M edges, levels 0 to 18), and 19 hole-check screenshots against a magenta sky have **0 enclosed
   sky pixels**.
2. **The governor makes streaming scale-independent, as ADR 0059 predicts, once terrain slopes are equal.** Mean
   demand is flat at 52 to 65 chunks/s across three altitude decades at s = 1. With the same slopes (s = 0.01, relief
   exponent 1) every phase is within 10 % of s = 1 in its mean and peak, except the descent's one-second peak (33 %).
   With ADR 0049's exponent 0.5, s = 0.01 terrain is steeper, and the near-ground peaks are 8 to 15 % higher (means
   up to 20 %), the descent's one-second peak 45 %.
3. **The rates are 3 to 4 times ADR 0053/0059's estimates**, and the near-ground speed floor is the expensive case.
   At 10 m above the ground at 100 m/s the selection asks for 450 to 520 chunks/s (one-second peaks 570 to 660). The
   M5 Max meshes 1,678 chunks/s on 17 threads with this generator, so it keeps up (at most 3 stale frames). A
   240 chunks/s budget, an M1-class stand-in, does not: the drawn terrain lags by seconds.
4. **ADR 0053's ±32 m linear samples break coarse levels.** Cells grow to 262 km at level 19. Saturated samples
   put every crossing at mid-edge and flatten gradients, so the planet looks terraced from orbit. A companded int16
   encoding (unchanged within ±16 m, logarithmic beyond) fixes it. It is in `engine/voxel` behind a mesher option.
5. **Relief exponent: 0.5 confirmed by eye** (images below). 0 is absurd at s = 0.01, 1 is a billiard ball.

## What was built

Throwaway, in the lab, plus one engine change behind an option. VCH-114 replaced the lab code below with
`engine/voxel` (`TerrainBody`, `LodSelector`, `ProgramDensity`) and the lab's engine-backed terrain view
(`apps/lab/src/terrain_view.cpp`, `scripts/terrain_descent.luau`); the spike's files are gone. See the
[voxel terrain spec](../../specs/voxel-terrain/spec.md).

| Piece | Where | Notes |
|---|---|---|
| Density fields | `apps/lab/src/sp09_terrain.{hpp,cpp}` | Planet: radius 6,371 km × s; planetary band on the direction (continents, 5 octaves; ridged mountains, 8 octaves; frequencies in 1/R, heights 3 km and 9 km × s^exponent); detail band in meters on the reference sphere (8 octaves, 600 m to 4.7 m, ±20 to 60 m, never scaled). Asteroid: 500 m sphere, 3D value noise in meters (4 + 6 octaves). Pure functions of the lattice point (ADR 0074 decision 1). Conservative box bounds from the octaves' Lipschitz constants |
| Octree | `Selector` in `apps/lab/src/sp09_spike.cpp` | Distance-based, λ = 4, as SP-08, pruned to boxes the bounds can't prove empty or solid, restricted with hash-set probes (O(n), not SP-08's O(n²)); the finer-face mask comes from the split set, so pruned neighbors need no probe |
| Streaming | `Spike::stream` | Each frame: select, mesh up to `budget` missing (chunk, finer mask) entries with `parallel_for` on the job system (ADR 0073), add them to `render::MeshStore` from the job (ADR 0072), evict what neither the drawn nor the target selection uses. **The drawn selection switches only when the whole target is meshed**, so it is always a complete restricted octree whose transition faces match its neighbors (ADR 0074 decision 2: a mask change is a remesh) |
| Camera-relative draws | extractor `sp09_chunks` | The camera is the render origin every frame (SP-03's finding); each chunk's `MeshDraw` translation is chunk origin − camera in double, then float. Vertices are chunk-relative floats computed from the mesher's lattice positions |
| Governor | `Spike::fly`, Simulation phase | Double-precision body-local camera; |v| = max(v_floor, h/τ) radially down to 100 m, then a 10 s skim at 100 m/s at 100 m AGL, a drop to 10 m and a 10 s skim at 10 m. Terrain following; the view is pitched a quarter of the way from the horizon to the nadir |
| Checks | `Spike::weld`, `checkpoints` | At each checkpoint: all drawn chunks' lattice positions welded, edges used once counted (exact crack test); screenshot; per-level counts. `VEACH_SP09_SKY=magenta` + `VEACH_SP09_COLORS=level` is the hole check |
| Companded samples | `engine/voxel`: `DistanceEncoding`, `encode_distance`, `decode_distance`, `TransvoxelOptions::encoding` | The mesher decodes codes to meters before interpolating and differencing. The linear path is unchanged (SP-08's committed hashes still pass) |
| Tests `[sp09]` | `tests/voxel_tests.cpp` | Encoding: linear within 16 m, monotonic, ≤ 0.06 % error; coarse 20 km ball at 128 to 512 m cells: watertight, mean normal·radial 1.000 companded vs 0.966 linear; committed hashes |
| Script | `apps/lab/scripts/sp09_descent.luau` | Camera, `Sp09Descent`, a dim constant sky; the environment picks the case |

To reproduce (from `apps/lab`, release build):

```sh
VEACH_SP09_SCALE=1 VEACH_SP09_OUT=/tmp/sp09 VEACH_SP09_PREFIX=s1 VEACH_SP09_BENCH=3 \
  ../../build/release/apps/lab/veach-lab --rhi metal --headless --frames 14000 --script scripts/sp09_descent.luau
# VEACH_SP09_SCALE=0.01 [VEACH_SP09_RELIEF=0|1]; VEACH_SP09_BODY=asteroid; VEACH_SP09_COLORS=level VEACH_SP09_SKY=magenta;
# VEACH_SP09_ENCODING=linear; --jobs 4 VEACH_SP09_BUDGET=4 [VEACH_SP09_ADAPTIVE=1] (the M1 stand-in)
```

Each run writes `<prefix>_frames.csv` (one row per frame), `<prefix>_summary.json` and the checkpoint screenshots, and
quits after the last skim.

## Screenshots

960×540, real-time renderer, Metal. Materials from the generator: lowland green, highland tan, rock gray, snow
white; steep triangles are rock. The sky is a dim constant blue-gray that also lights the shade, so snow in shadow is
close to the sky's color: those patches are slopes facing away from the sun, not holes (the hole-check images
settle it).

| Checkpoint | s = 1 (R = 6,371 km) | s = 0.01 (R = 63.7 km), exponent 0.5 | Asteroid (1 km) |
|---|---|---|---|
| Orbit (h = R; 200 km at s = 0.01; 5 km) | [s1_orbit](sp-09/s1_orbit.png): 90 chunks, L16 to L18 | [s001_orbit](sp-09/s001_orbit.png): 8 chunks | [asteroid_orbit](sp-09/asteroid_orbit.png) |
| 100 km | [s1_100km](sp-09/s1_100km.png) | [s001_100km](sp-09/s001_100km.png) | |
| 10 km | [s1_10km](sp-09/s1_10km.png) | [s001_10km](sp-09/s001_10km.png) | |
| 1 km | [s1_1km](sp-09/s1_1km.png) | [s001_1km](sp-09/s001_1km.png) | [asteroid_1km](sp-09/asteroid_1km.png) |
| 100 m | [s1_100m](sp-09/s1_100m.png): 1,591 chunks, 3.3 M triangles | [s001_100m](sp-09/s001_100m.png): 1,120 chunks | [asteroid_100m](sp-09/asteroid_100m.png) |
| 10 m, after a 10 s skim | [s1_10m](sp-09/s1_10m.png): 1,739 chunks, 3.5 M triangles | [s001_10m](sp-09/s001_10m.png) | (the 10 m view faces a slope in shadow: uniformly dark) |
| Hole check (level colors, magenta sky) | [hole_s1_100km](sp-09/hole_s1_100km.png), [hole_s1_10m](sp-09/hole_s1_10m.png) | [hole_s001_100m](sp-09/hole_s001_100m.png) | [hole_asteroid_100m](sp-09/hole_asteroid_100m.png) |

What I saw:

- No gap at any seam or level. The level bands in the hole-check images (L0 red, L1 orange, L2 green, L3 blue, L4
  purple, L5 yellow, ...) are continuous. A script counted sky-colored pixels not connected to the sky in all 19
  hole-check images: 0 in each.
- LOD boundaries show as faint circular shading steps (coarser levels have smoother normals), for example the arcs in
  [s001_100m](sp-09/s001_100m.png). It is popping, not a crack. Morphing or detail normals would hide it.
- A few isolated dark specks at far seams, as SP-08 saw: flat transition triangles shaded from behind. SP-08's
  pinholes with several primitives per chunk did not appear: these meshes carry up to five material slots, and the
  magenta check found no sky through them.
- At s = 1 the terrain near the ground is the detail band on a nearly flat planetary band, because 9 km mountains
  have 100 km wavelengths there. At s = 0.01 the same mountains, 880 m tall on 1 km wavelengths, fill the view.

### Why the samples are companded

[linear_s001_100km](sp-09/linear_s001_100km.png) is the s = 0.01 planet from 100 km with ADR 0053's encoding
(1/1024 m, clamped to ±32 m). [s001_100km](sp-09/s001_100km.png) is the same view with companded codes. The visible
levels have 512 m to 2 km cells, so almost every sample is clamped. Crossings land mid-edge and central differences
see ±32 m steps, which gives the voxel terraces. The companded code is linear (1/1024 m) to ±16 m, then
16384 + K·ln(1 + (|d| − 16)/D) with the same slope at the knee, reaching 32767 at 10,000 km (steps ≈ 0.1 % of the
distance). The mesher decodes before interpolating. The sign, LOD0 precision and the int16 size are unchanged, and a
sample is still a pure function of position (ADR 0074).

## Streaming

"Requests" are (chunk, finer-face mask) entries the selection asks for that the previous frame's did not: new chunks
plus remeshes of chunks whose neighbor changed level. "Jobs" are the entries actually sampled and meshed. Peaks are
the largest sum over any 60 frames (1 s; one fixed step per frame in headless runs). Budget: 32 jobs per frame on 17
threads unless noted.

| Requests/s, mean (1 s peak) | s = 1 | s = 0.01, e = 0.5 | s = 0.01, e = 1 | s = 0.01, e = 0 | Asteroid |
|---|---|---|---|---|---|
| Descent, 10,000 to 1,000 km | 60 | — | — | — | — |
| 1,000 to 100 km | 56 | (10, from 200 km) | (10) | (23) | — |
| 100 to 10 km | 52 | 46 | 46 | 91 | — |
| 10 to 1 km | 65 | 76 | 66 | 174 | 14 (5 to 1 km) |
| 1 km to 100 m | 92 | 106 | 90 | 284 | 105 |
| **Whole descent** | **63 (201)** | **66 (292)** | **59 (268)** | 155 (784) | 58 (319) |
| **Skim, 100 m AGL, 100 m/s** | **235 (341)** | **281 (368)** | **228 (316)** | 1,150 (1,662) | 232 (351) |
| **Drop + skim, 10 m AGL, 100 m/s** | **446 (574)** | **522 (661)** | **429 (519)** | 1,458 (2,284) | 533 (704) |
| Remeshes (share of requests) | 38 to 49 % | 35 to 49 % | 36 to 50 % | 40 to 48 % | 39 to 52 % |
| Jobs; with no triangles | 10,529; 8 % | 10,745; 13 % | 8,962; 9 % | 31,520; 22 % | 9,042; 12 % |
| Descent time (τ ln(h₀/500 m) + 4 s) | 51.2 s | 33.9 s | 33.9 s | 33.9 s | 15.5 s |
| Most chunks drawn; triangles | 1,773; 3.6 M | 1,334; 2.6 M | 1,175; 2.3 M | 2,282; 4.5 M | 671; 1.2 M |
| Drawn selection stale, max | 3 frames | 2 | 2 | 74 | 2 |

Across scales:

| s = 0.01 vs s = 1 | e = 1 (same slopes): mean / peak | e = 0.5 (ADR 0049): mean / peak |
|---|---|---|
| Descent | −7 % / +33 % | +4 % / +45 % |
| Skim at 100 m | −3 % / −7 % | +20 % / +8 % |
| 10 m | −4 % / −10 % | +17 % / +15 % |

- **The governor does its job.** At s = 1, demand stays at 52 to 65 chunks/s from 10,000 km down to 1 km. At equal
  slopes, s = 0.01 matches it within 10 % everywhere except the descent's one-second peak. That peak is the last
  seconds of the descent: the 100 m/s floor from 500 m to 100 m, where N grows as v/h. It depends on the terrain
  under that spot, which is a different place at each scale. The criterion "peak within 20 % across scales" holds for
  every mean and for the near-ground peaks. It fails for the descent's one-second peak, and the cause is terrain, not
  scale.
- **The relief exponent is a streaming cost.** Planetary relief × s^0.5 makes s = 0.01 slopes √(1/s) = 10 times
  steeper than s = 1's at the same angular wavelength. Rough terrain crosses more chunks per level. At e = 0.5 it
  costs 15 to 20 % more near the ground than e = 1. At e = 0 it costs 2.5 to 5 times more, and the 1,920/s budget
  fell behind (74 stale frames).
- **Per level, about 80 to 130 surface chunks**, not the 64 ADR 0059 assumed, at every level from 1 to 17 (checkpoint
  logs). λ = 4's ring reaches about 4 chunk edges out, and the surface crosses more than one layer of chunks. With
  remeshes at about 40 % of requests, the high band runs at about **60/s ≈ 300/τ**, not ADR 0059's 96/τ ≈ 19/s.
- **Near the ground, N ≈ 2.4 to 2.8·v at 100 m AGL and 4.5 to 5.2·v at 10 m AGL** (v in m/s), not ADR 0053's 1.5·v.
- **Remeshes are about 40 % of all jobs** (ADR 0074 decision 2). Each one re-samples the whole chunk. Keeping the
  grid samples of resident chunks (171 KB each as stored here) would reduce a remesh to its new face samples (about
  30 % of a chunk per face).

### Bounds decide the waste, not the mesher

Chunks the bounds cannot prove empty are sampled. One Lipschitz bound per box (ADR 0053's "min/max skipping" with
nothing cached) left most jobs empty near the ground, because the detail band's fine octaves saturate over a 16 m
box. Splitting the box and bounding the pieces fixes it:

| Box bounds, s = 0.01 run | Jobs | With no triangles | Selection ms, median (max) |
|---|---|---|---|
| One bound per chunk (earlier site, 0.03 rad from the peak) | 44,595 | 77 % | — |
| Split 3 times (pieces of 4 cells) | 19,144 | 46 % | 0.8 (2.8) |
| Split 5 times | 12,143 | 20 % | 0.7 (10) |
| **Split 6 times (half-cell pieces), used above** | **10,727** | **13 %** | **0.6 (28)** |

Bound results are cached per node, so the cost falls on the frames that reach new nodes (the maxima). The real
selector would run them on workers.

### Cost per chunk and the budget

`VEACH_SP09_BENCH`: the 1,591 chunks drawn at the s = 1 100 m checkpoint, re-sampled and re-meshed on the job
system, best of 2 to 5 runs, machine otherwise idle.

| Threads | Chunks/s | Sample ms per chunk (CPU) | Mesh ms per chunk |
|---|---|---|---|
| 1 | 129 | 7.6 | 0.08 |
| 4 | 457 | 8.5 | 0.10 |
| 17 (default) | **1,678** | 9.8 | 0.13 |
| Asteroid, 17 | 3,010 | 5.4 | 0.12 |

Building meshlets in `MeshStore::add` adds 0.6 ms per chunk, on the job. Sampling is the cost: about 47,000 samples
per chunk (35³ plus 3 × 67² per finer face) × 21 octaves of scalar double value noise, about 7 ns per octave. SP-06's
batched float interpreter does 4.5 ns per octave, so a field graph would sample about 1.6 times faster.

- **M5 Max: within budget.** The worst one-second demand at e = 0.5 (661/s at 10 m) is 40 % of 1,678/s. With 32 jobs
  per frame the drawn selection was at most 3 frames stale, and at most 31 chunks behind.
- **M1-class: not within budget near the ground.** An M1 has about 3 job threads, each about 0.55 times an M5 Max
  core (an estimate, not measured), so about 200 chunks/s with this generator. The stand-in, `--jobs 4` with
  4 jobs per frame (240/s), kept up with the descent (≤ 34 stale frames), but in the skims the drawn terrain lagged
  up to 94 to 370 frames at 100 m AGL and 685 to 1,024 frames at 10 m: it never caught up. Lowering λ while stale
  (`VEACH_SP09_ADAPTIVE=1`, down to λ = 2) cut that to 40 to 167 and 132 to 254 frames. Changing λ remeshes
  everything, though, so it adds bursts (up to 1,094 requests/s). The all-or-nothing switch needs a finer policy (per
  subtree) for overload.
- **Frame time.** The spike blocks the frame on its jobs, a simplification. Median 2.6 to 4.6 ms, p95 23 to 29 ms,
  p99 33 to 44 ms on 17 threads with 32 jobs per frame: the p95 frames are the meshing waves. Real streaming must run
  its jobs across frames (TaskGroup, ADR 0073) and only install results on the main thread.

## Do ADRs 0053, 0059 and 0049 hold?

- **0053, the representation: holds.** One density field, a body-local octree from 16 m to 4,194 km chunks
  (levels 0 to 18 drawn, root 19 at s = 1), Transvoxel seams, camera-relative extraction and "never a hole". This
  spike also confirms ADR 0074's three amendments in streaming: LOD-independent samples, remesh on neighbor change,
  and positions from the lattice.
- **0053, the sample encoding: amend.** The ±32 m linear band cannot carry coarse levels; use the companded code (or
  an equivalent per-sample scale that is still a function of position only). It is in every save, so this is the
  time to decide. SP-12's brushes (VCH-088) combine LOD0 codes in integer arithmetic; companded codes are the same
  within ±16 m, so they need `encode_distance` only where an edit writes distances beyond that.
- **0053, the rate contract and B: amend the constants.** N ≈ 60/s in the high band (not 96·v/h ≈ 19/s), and near
  the ground ≈ 2.5·v at 100 m AGL and ≈ 5·v at 10 m AGL (not 1.5·v). B ≈ 300 chunks/s on a base M1 is not reached
  with this generator (≈ 200/s projected). The "reopen if" clause triggers only if a faster generator (SP-06's
  batched interpreter, about 1.6 times) plus cached samples for remeshes (about 1.3 times) still falls short. Those
  must be measured first; GPU far-field meshing is not needed yet.
- **0059, the cap law: holds.** Descent times are τ·ln(h₀/h₁) plus the floor segment, as computed. Demand is flat
  across altitude and scale at equal slopes. The floor: at v_floor = 100 m/s, 10 m AGL needs about 450 to 520
  chunks/s (peaks 660). That is fine on an M5 Max and about 2.5 times an M1's projected budget. Decision 1 below.
- **0049, relief: holds, with a cost.** Exponent 0.5 is confirmed by eye. It makes small planets 15 to 20 % more
  expensive to stream near the ground than exponent 1. Exponent 0 multiplies that by 2.5 to 5, so creators should not
  get it without a warning.

## Relief exponent at s = 0.01

The same planet (seed, site and camera path); only the planetary band's exponent changes (0: real heights, 11 km
peaks on a 64 km planet; 0.5: 1.1 km; 1: 110 m). Snow and rock thresholds scale with the band, so the white marks the
same ridges in each.

| | Exponent 0 | **Exponent 0.5** | Exponent 1 |
|---|---|---|---|
| 100 km | [relief0_100km](sp-09/relief0_100km.png) | [s001_100km](sp-09/s001_100km.png) | [relief1_100km](sp-09/relief1_100km.png) |
| 10 km | [relief0_10km](sp-09/relief0_10km.png) | [s001_10km](sp-09/s001_10km.png) | [relief1_10km](sp-09/relief1_10km.png) |
| 1 km | [relief0_1km](sp-09/relief0_1km.png) (inside a cliff) | [s001_1km](sp-09/s001_1km.png) | [relief1_1km](sp-09/relief1_1km.png) |

- **0:** a spiky burr from orbit. Mountains taller than the 10 km checkpoint, slopes far beyond vertical (the
  heightfield stretches into streaks), and the 1 km camera ends up inside a wall. Not a planet.
- **0.5:** from 100 km the ranges read as relief on the limb and the surface. From 1 km the snow massif stands up
  from the lowland, while from 10 km the planet still reads as a sphere.
- **1:** a smooth ball from orbit, and from 1 km the "mountains" are gentle swells. It is realistic for a 64 km body,
  but the planet loses its character at small scale.

**Recommendation: keep 0.5 as the default** (ADR 0066's working value). It keeps planets recognizable as the same
place across scales without e = 0's breakage. Its cost is the 15 to 20 % near-ground streaming above, which the
budget absorbs on M5 Max.

## Decisions for the dev lead

1. **The near-ground speed floor.** Recommendation: keep v_floor = 100 m/s as the gameplay constant (it is
   simulation, ADR 0059). Below the budget, a machine renders coarser LOD near the ground; it never slows the ship.
   Collider chunks within 32 m are the only hard deadline. At 100 m/s that is about 60 LOD0 chunks/s (estimated, not measured: a band 5 chunks wide and 2 deep), which fits any machine. The
   alternative is a lower floor near the ground, about 40 m/s below 50 m AGL, sized for the projected M1 budget.
   Re-measure on M1/M3 Pro hardware with the field-graph generator before choosing.
2. **Adopt the companded sample encoding** in ADR 0053's sample format (a new ADR amending 0053): same int16, same
   meaning within ±16 m.
3. **Replace ADR 0053/0059's rate constants** with the measured ones above, and set `TerrainStreamingBudget` from
   chunks/s measured per tier. The M5 Max does 1,678/s on 17 threads with this generator.
4. **Relief exponent 0.5: confirm.**

## Follow-ups

- Streaming jobs across frames (TaskGroup) instead of blocking the frame; a per-subtree switch so an overloaded
  stream degrades locally instead of freezing the whole drawn selection.
- Keep grid samples for resident chunks (or the finest few levels) so a neighbor's level change resamples only the
  new transition faces; measure against the 40 % remesh share.
- Sample with SP-06's batched field-graph interpreter instead of this scalar hand-written field, and measure B on the
  M1 class.
- Geomorphing or detail normals to hide the LOD shading steps; the flat transition triangles that shade dark (SP-08).
- Cache box bounds per body (ADR 0053's min/max pyramid) instead of re-deriving them, if the selection's first-visit
  cost (up to 28 ms) shows in Tracy once it runs on workers.
