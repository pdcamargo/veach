# Seams sweep: octant planes, poles and chunk corners (VCH-116)

The owner asked whether the planet and its physics work at the seams: "top left, top right, top center, bottom
center", thinking of the faces and corners of a cube-sphere. Our terrain has no cube faces. It is a 3D density field
in a body-local octree ([ADR 0053](../../adr/0053-terrain-density-field.md),
[0074](../../adr/0074-amendments-from-sp-08.md), [0079](../../adr/0079-amendments-from-sp-09.md)), and that octree
has seams of its own, which this sweep tests:

- **The octant planes** x = 0, y = 0 and z = 0 through the planet's center. The octree's eight roots meet there, so
  every level has a chunk boundary on them. Lattice coordinates change sign there, which is where integer division
  and rounding of negative numbers go wrong.
- **The poles.** The body-fixed frame's spin axis is +Y, so each pole lies on two octant planes at once, a line where
  4 chunks meet at every level. "North" and "east" are undefined there.
- **The cube-corner directions** (±1, ±1, ±1)/√3, where a cube-sphere's coarsest faces would meet.
- **Chunk corners**, where 8 LOD0 chunks meet.
- **Procgen inputs that are cube-mapped**: SP-07's climate and biome map and SP-06's field graph over face regions.

**Verdict: nothing breaks at these places.** The terrain welds with 0 open edges at all eight sites, across four
levels of detail (three at the corner sites). The field is continuous across the octant planes. SP-12's ball falls into its pit and comes to rest
at every site. A character walks across each octant plane, through an 8-chunk corner and over both poles without
losing the ground once in about 1.07 M steps (10 km per walk), its up stays radial, and its heading stays
continuous over the poles. SP-13's craft lands at the north pole as well as at 30°. The cube-mapped inputs show no
seam at the face edges.

**One thing does not work like normal, and it is everywhere, not only at these places.** A walking character
crossing from one chunk's collider to the next can get a one-step vertical bump, up to 1.2 m/s more than the same
triangles give in a single collider (|Δv| per step near chunk borders, p99: 0.6 to 1.3 m/s with chunk colliders,
0.2 to 1.2 m/s in one collider). Jolt treats a collider's border
edges as sharp, because the triangles past them belong to another body. This is very likely what SP-13 saw as 13 to 14
contact losses a day at chunk seams ([ADR 0080](../../adr/0080-amendments-from-sp-13.md)). A fix is measured below:
cooking each chunk's collider with its neighbors' border triangles as adjacency only matches the single collider
on every walk. It
goes into `engine/physics` (VCH-113).

![A ball resting in a pit dug at the north pole, where four chunk columns meet](seams/pole_pit.png)

## Per site

| Site | Where | Terrain: chunks meshed by level, triangles, open edges | Across its octant planes | Dig and collide: the ball at the end | Walk | Verdict |
|---|---|---|---|---|---|---|
| north_pole | +Y, on x = 0 and z = 0 | L0 16, L1 12, L2 12, L3 5; 116,340; **0** | 143,029 shared samples, 0 differ; step at the plane ≤ 0.050 m (within one side ≤ 0.178 m) | 3.98 m deep, 0.007 m off axis, at rest | diagonally over the pole, and along the z = 0 boundary plane: 0 contact losses | works |
| south_pole | −Y, on x = 0 and z = 0 | 16, 12, 12, 5; 114,985; **0** | 143,029, 0 differ; ≤ 0.035 m (≤ 0.190 m) | 3.98 m, 0.008 m | diagonally over the pole: 0 | works |
| plane_x | (0, −0.6, −0.8), on x = 0 | 38, 33, 34, 6; 234,369; **0** | 107,231, 0 differ; ≤ 0.043 m (≤ 0.152 m) | 3.98 m, 0.058 m | across x = 0: 0 | works |
| plane_y | (−0.6, 0, −0.8), on y = 0 (the equator) | 37, 32, 34, 5; 236,743; **0** | 107,471, 0 differ; ≤ 0.044 m (≤ 0.162 m) | 3.98 m, 0.058 m | across y = 0: 0 | works |
| plane_z | (−0.6, −0.8, 0), on z = 0 | 37, 34, 34, 5; 235,716; **0** | 107,471, 0 differ; ≤ 0.043 m (≤ 0.154 m) | 3.97 m, 0.013 m | across z = 0: 0 | works |
| corner_neg | (−1, −1, −1)/√3 | 64, 63, 54; 385,842; **0** | none (on no plane) | 3.96 m, 0.071 m | straight east: 0 | works |
| corner_pos | (1, 1, 1)/√3 | 66, 63, 54; 388,410; **0** | none | 3.95 m, 0.085 m | not walked | works |
| lon180 | (−1, 0, 0): equator, longitude 180°, on y = 0 and z = 0 | 16, 12, 12, 5; 113,391; **0** | 143,029, 0 differ; ≤ 0.043 m (≤ 0.178 m) | 3.98 m, 0.000 m | across both planes at once: 0 | works |
| An 8-chunk corner | (32, −3,822,624, −5,096,784) m, 5 mm from the surface, near plane_x | — | — | — | straight through the corner: 0 | works |
| sp12 (control) | (0.6, 0.8, 0) | — | — | 3.98 m, 0.002 m: SP-12's committed hashes, unchanged | — | unchanged |

The cube-corner directions are not special for this octree: its corners are where the octant planes meet, at the
planet's center, and the coarse chunk boundaries a surface point sits on depend on where it is, not on its direction.
The two corner sites have no octant plane through them and behave like any other place. They mesh more chunks only
because the surface crosses the lattice diagonally there.

## What was built

Throwaway test code, plus a site parameter in SP-12 and a latitude parameter in SP-13. No engine changes: no engine
bug was found.

| Piece | Where | Notes |
|---|---|---|
| Sites, the multi-level weld, continuity, walks | `tests/seams/seams.{hpp,cpp}` (`veach-seams` library) | Sites by bisection on SP-12's field, rounded to the millimeter (a coordinate that is 0 in the direction stays exactly 0). Tangent frame: east = ŷ × up; at the poles, where that is 0, east is +X |
| The walker | `tests/seams/seams_walk.cpp` | SP-13's character controller (`CharacterVirtual`, up approached toward −ĝ, heading parallel-transported) on SP-12's cooked LOD0 colliders. Five collider layouts (below). The only seams file with Jolt |
| Cube-map checks | `tests/seams/seams_cubemap.hpp` | Neighbor pairs on the 64² × 6 map, within a face and across an edge; SP-06's terrain graph over six face regions |
| Tests `[seams.AC1]` to `[seams.AC6]` | `tests/seams/seams_tests.cpp` | In `veach-tests`, the fast set. One test case per part, so ctest runs them in parallel: 6.3, 5.1, 4.7, 0.8 and 0.4 s in Debug on an M5 Max |
| `veach-bench-seams` | `tests/seams/seams_bench.cpp` | `lod`, `dig`, `walk --legs N`, `cubemap`, `pole`, `trace`, `shots`: the tables and screenshots below |
| SP-12 site parameter | `tests/sp12`: `Site`, `site_offset_mm(site, …)`, `ScenarioConfig::site`, `dig_log(site)` | The default site reproduces SP-12's committed hashes. `sp12_jolt.hpp` shares the cooked shape with the walker |
| SP-13 latitude | `tests/sp13`: `Config::latitude`, `make_planet(scale, latitude)` | Default 30°: SP-13's hashes are unchanged |

## 1. Terrain: weld and continuity

`veach-bench-seams lod`: around each site, a restricted octree from engine `select_chunks` (SP-08's selection,
λ = 2) over 3³ roots of 128 m, focus 2 m above the site. Every leaf the field's bound can't prove empty or solid is
sampled and meshed with the Transvoxel mesher, faces toward finer neighbors at full resolution. Then SP-08's exact
weld check runs on every chunk: vertices welded by their lattice position, every edge used once away from the region
boundary is a hole. Table above: **0 open edges and 0 non-manifold edges at every site**, with transition triangles
on every level change (2,080 to 3,452 per site).

**Across the octant planes**, two checks:

- **Shared samples.** Every grid sample and full-resolution face sample within one cell of the plane is collected
  from every chunk that holds it, on both sides and at every level. Each lattice point held by two or more chunks
  must hold the same sample: 107,231 to 143,029 such points per site, **0 differ**.
- **The field itself.** Along 41 lines through the plane where it meets the surface, 17 samples each: the largest
  step between neighbors across the plane is 0.03 to 0.05 m, smaller than between neighbors on one side
  (0.15 to 0.19 m) and far under the field's bound of 1.45 m per 0.5 m step. 1 µm either side of the plane, the
  unquantized distance differs by at most 2e-13 m. The noise uses `floor`, not truncation, for negative coordinates
  (SP-12's, SP-09's and SP-08's fields all do), so there is no doubled cell at 0.

The edit path is sign-correct too: `brush_box`, `touched_chunks` and `chunk_of_mm` use floor division, and every dig
below runs across negative coordinates with 0 shared-sample mismatches.

![The multi-level octree across the x = 0 plane: L0 red, L1 orange, L2 green, L3 blue; the x < 0 half darker](seams/lod_plane_x.png)

## 2. Physics

### Dig and collide

`veach-bench-seams dig`: SP-12's whole flow at each site. A 5³-chunk region is loaded and colliders are cooked. A
0.4 m ball is dropped. The six-op edit log runs (a pit, a deeper sphere, a mound, a dent in it, a cavity in rock,
then a sump at tick 600), with colliders installed at t + 2. Results in the per-site table: **0 open edges, 0
shared-sample mismatches, no op outside the region; the ball lands 0.32 to 0.39 m above the original surface (its
radius is 0.4 m), sits 2.58 to 2.60 m down in the pit before the sump, and ends 3.95 to 3.98 m down in the sump, at
most 8.5 cm off the axis, at rest**, at every site. SP-12's own site gives 3.98 m. At four sites the ball was still
rolling slowly at tick 600 instead of asleep, so the sump install did not have to wake it there. SP-12's site tests
the wake.

### Walks

`veach-bench-seams walk --legs 200`: a character walks a 50 m great-circle line at 5 m/s, back and forth 200 times.
Each leg turns to face its end point and stops when it passes it. The straight steps are measured: about 118,000
per walk, 10 km.

| Walk | Chunk crossings | Octant-plane crossings | Contact losses | Feet above the colliders | Up vs radial, max | Heading change per step, max | Compass heading, largest one-step jump |
|---|---|---|---|---|---|---|---|
| across x = 0 | 1,000 | 200 | **0** | −1.5 to 59 mm | 1.2e-7 rad | 1.3e-8 rad | 0 |
| across y = 0 | 801 | 200 | **0** | 1.0 to 91 mm | 1.2e-7 | 1.3e-8 | 0 |
| across z = 0 | 800 | 200 | **0** | −0.8 to 59 mm | 1.2e-7 | 1.3e-8 | 0 |
| through an 8-chunk corner | 1,198 | 0 | **0** | −0.8 to 45 mm | 1.2e-7 | 1.3e-8 | 0 |
| over the north pole, diagonally | 1,015 | 400 | **0** | −1.5 to 53 mm | 1.2e-7 | 1.3e-8 | **2.89 rad** |
| over the north pole, along the z = 0 boundary | 1,190 | 795 | **0** | 0.0 to 44 mm | 1.2e-7 | 1.3e-8 | **3.14 rad** |
| over the south pole, diagonally | 1,198 | 400 | **0** | −1.3 to 45 mm | 1.2e-7 | 1.3e-8 | **2.41 rad** |
| across longitude 180° (y = 0 and z = 0 at once) | 1,199 | 400 | **0** | −1.3 to 60 mm | 1.2e-7 | 1.3e-8 | 0 |
| at (−1, −1, −1)/√3 | 800 | 0 | **0** | −1.6 to 60 mm | 1.2e-7 | 1.3e-8 | 0 |

- **No contact losses in 1,068,530 straight steps** (SP-13: 13 to 14 in 5.18 M on its analytic sphere, never more than
  1 cm). Feet heights are along −up from the capsule's lowest point. The tens of millimeters are the 0.3 m capsule
  bottom resting on slopes, not air. Feet were never more than 5.4 mm into the colliders in any variant, so there
  was no fall-through.
- **Up stays radial** to 1.2e-7 rad, the lag of SP-13's 0.15 s time constant at 5 m/s. **The heading is continuous
  over both poles**: the forward vector turns by at most 1.3e-8 rad per step, which is the parallel transport along
  a 6,371 km sphere. A compass heading measured from geographic north flips by up to π in one step at a pole.
  Anything that steers by north breaks there, and the character controller doesn't. Headings must stay transported
  vectors, as SP-13 built them.
- **|Δv| per step** (velocity from positions, the leg starts excluded) is in the next section.

### Bumps at chunk collider borders

Comparing |Δv| within 0.5 m of a chunk boundary plane with |Δv| elsewhere doesn't separate the terrain's own creases
from collider effects. So each walk also ran on the same triangles in other collider layouts:

- **chunks**: SP-12's colliders, one `MeshShape` per LOD0 chunk (what ADR 0058 specifies).
- **merged**: the control. All chunks in one `MeshShape`, vertices welded by lattice position: the same triangles,
  no collider borders.
- **edges**: chunks, with `CharacterVirtualSettings::mEnhancedInternalEdgeRemoval`.
- **ring**: each chunk's collider also holds its neighbors' triangles that touch its box (duplicated, colliding).
- **adjacency**: the same ring, but the ring triangles carry a second material, and the character's contact
  listener rejects contacts with it. They only give the chunk's border edges their neighbors when Jolt marks
  active edges at cook time.

| Walk: \|Δv\| near chunk borders, p99 / max, m/s | chunks | merged | edges | ring | **adjacency** |
|---|---|---|---|---|---|
| across x = 0 | 1.12 / 1.49 | 0.46 / 0.74 | 0.72 / 1.32 | 1.04 / 1.51 | **0.46 / 0.74** |
| across y = 0 | 0.72 / 1.29 | 0.33 / 0.63 | 0.62 / 1.29 | 0.83 / 1.47 | **0.33 / 0.63** |
| across z = 0 | 0.62 / 1.25 | 0.27 / 0.50 | 0.59 / 1.04 | 0.94 / 1.32 | **0.27 / 0.52** |
| through an 8-chunk corner | 0.73 / 0.99 | 0.50 / 0.97 | 0.68 / 0.89 | 0.91 / 1.26 | **0.50 / 0.98** |
| over the north pole, diagonally | 0.93 / 1.52 | 0.45 / 0.70 | 0.78 / 1.54 | 0.99 / 1.33 | **0.45 / 0.70** |
| over the north pole, along z = 0 | 1.32 / 1.90 | 1.16 / 1.90 | 0.81 / 1.52 | 1.18 / 1.86 | **1.16 / 1.89** |
| over the south pole, diagonally | 0.71 / 1.70 | 0.72 / 1.71 | 0.59 / 1.11 | 1.24 / 1.67 | **0.72 / 1.71** |
| across longitude 180° | 0.98 / 1.20 | 0.23 / 0.31 | 0.60 / 0.72 | 0.99 / 1.25 | **0.23 / 0.32** |
| at (−1, −1, −1)/√3 | 1.00 / 1.22 | 0.48 / 0.60 | 0.99 / 1.22 | 1.11 / 1.42 | **0.48 / 0.61** |
| Away from borders, p99 (all variants alike) | 0.31 to 1.22 | | | | |

- **What it is.** `veach-bench-seams trace` writes every step. While the chunk and merged runs still follow the same
  path to the micrometer, nearly every step where the chunk run's |Δv| exceeds the merged run's by more than 0.1 m/s
  is within 0.5 m of a chunk boundary: 17 of 18 on the corner walk, 26 of 27 over the north pole, 28 of 29 at
  longitude 180°. The extra is almost entirely vertical, up to 1.2 m/s in one step. The capsule catches on a
  border edge for a frame and is pushed up: a hitch a player would feel, not a fall. It happens at chunk borders
  everywhere, special place or not. On some walks the terrain's own creases are as large (the south-pole walk shows
  no difference).
- **Why.** Jolt marks a mesh edge active (sharp) unless the triangle across it is in the same `MeshShape` and the
  angle is small. A chunk collider's border edges have no neighbor in their own shape, so all of them are active.
  Enhanced internal edge removal works within one body pair, so it helps only a little across bodies. Duplicating
  the ring makes it worse: the ring's own outer edges are borders of the neighbor's shape, so the bump moves one
  cell in.
- **The fix, measured:** adjacency rings match the merged control on every walk, to 0.003 m/s at p99 and
  0.02 m/s at the max, and they change nothing away from the borders. Cost: 9 to 11 % more triangles to cook (93,984
  to 103,753 on the x = 0 walk's 43 colliders), whose contacts the listener rejects. No extra recooking: a ring triangle comes from the neighbor's samples within one cell of the shared face, which
  lie in this chunk's apron, so any edit that changes them already marks this chunk dirty (`touched_chunks` includes
  the apron).
- **Not measured:** rigid bodies. SP-12's ball rolls over chunk borders too. Their contact listener is per body
  pair, so they need another way to ignore ring triangles, for example `ContactSettings::mIsSensor` set per contact
  in `OnContactAdded`. That belongs to the same `engine/physics` work.

### SP-13 at the pole

`veach-bench-seams pole`: SP-13's autopilot landing (descent from 1.15 × the atmosphere top, hover 5 s at 50 m, land),
with the walker circling the pad 200 m out, with the pad moved toward the north pole:

| Pad latitude | s = 0.01: hover band, touchdown, distance from the pad | s = 1 |
|---|---|---|
| 30° (SP-13) | 49.09 to 49.94 m, 0.501 m/s, 0.015 m | 49.09 to 49.94 m, 0.501 m/s, 0.010 m |
| 89° | same, 0.017 m | same, 0.010 m |
| 89.99° | same, 0.020 m | same, 0.010 m |
| 89.9999° | same, 0.031 m | same, 0.010 m |
| 90° | same, 0.031 m | same, 0.007 m |

The walker stays supported at every latitude, between −0.04 and 10.0 mm above the sphere. `[seams.AC6]` commits the
s = 0.01, 90° landing.

SP-13's flight assist and its `CraftView` take east as ŷ × up, normalized. At a point exactly on the axis that is
0 / 0. The 90° pad is 6e-17 off the axis, because `dmath::cos(π/2)` is not 0, so it never happens here. Near the
axis that east swings around as the craft drifts, so "strafe east" means nothing at a pole. The landing doesn't care:
the autopilot steers back to the pad either way. For `engine/physics` and the flight model: build a craft's
horizontal axes from a transported frame (its own heading, as the characters do), never from the spin axis.

## 3. Cross-platform

The hashes are committed in `tests/seams/seams_tests.cpp`, folded per part. Each test's INFO lines print the per-site
values when one differs:

| Test | What the hash covers | macOS arm64 |
|---|---|---|
| `[seams.AC1]` | Every leaf's mesh at the 8 sites (3³ roots of 64 m in the test) | `0x97715b03a237437d` |
| `[seams.AC2]` | Base samples, edited samples, meshes, cooked colliders, the install and the ball's 1,200-step chain, 8 sites | `0x3cfd9dbf51875426` |
| `[seams.AC3]` | The 9 walks, every step's character state (2 legs each) | `0xcdb2b78d799d3a05` |
| `[seams.AC4]` | The merged and adjacency runs of 3 walks | `0xa3aba7a8cbc3268d` |
| `[seams.AC6]` | SP-13's landing at the north pole, s = 0.01 | `0xcd52b59f83d39b34` |

Debug and Release give the same values on macOS arm64. Windows x64 CI checks them on the next push. AC2 extends
SP-12's cross-platform claim to the sites with negative coordinates. AC3, AC4 and AC6 extend SP-13's to
`CharacterVirtual` on voxel colliders. `[seams.AC5]` (the cube maps) has no hash: it checks gradients.

## 4. Cube-mapped procgen inputs

**Nothing the terrain or its materials read today is cube-face sampled.** SP-09's and SP-12's fields compute height,
noise and material from the 3D position or direction at each lattice point. Two spikes did use cube maps, and
`veach-bench-seams cubemap` checks both for a seam at the face edges. It compares |Δvalue| / angle between
neighboring cells across an edge with the same quantity between neighbors on one face next to an edge (cells there
are as small as the pairs across it, about half the size of central ones):

| Input | Within a face, next to an edge: p99 / max | Across an edge: p99 / max |
|---|---|---|
| SP-07 altitude (m/rad) | 25,920 / 32,150 | 24,560 / 28,670 |
| SP-07 temperature, from the Luau hook (K/rad) | 163.7 / 195.1 | 142.0 / 186.3 |
| SP-07 precipitation (mm/yr/rad) | 21,910 / 47,710 | 21,490 / 41,080 |
| SP-07 biome: pairs that differ | 11.9 % | 9.5 % |
| SP-06 terrain graph, 64² per face | 19.98 / 33.01 | 25.08 / 31.31 |
| SP-06 terrain graph, 256² per face | 33.43 / 59.09 | 36.77 / 62.19 |

**No numeric seam.** Both inputs are functions of the 3D direction, and the face grid only decides where samples
lie, so values across an edge change no faster than along a face (ratios 0.87 to 1.26). 804 points exactly on the 12
edges, evaluated through SP-06 from both faces' parameterizations, give the same float position and the same value
bit for bit. With 0 and ±1 axes, c + u·e_u + v·e_v is exact in float.

Three things for whoever builds the climate stage:

- **A consumer can still make a seam.** Bilinear filtering within one face can't blend across an edge. Read these
  maps with seamless cube filtering or a one-cell apron per face, or evaluate the function at the sample point, as
  the terrain does.
- **Cells are not equal in area.** Neighbor spacing is 0.0147 rad at face edges and corners against about 0.03 rad at
  face centers (gnomonic projection), so resolution varies by about 2× across a face. That is not a seam.
- **SP-07's precipitation is jittered by `ctx.random(1, i)`, keyed on the cell index.** That is white noise of ±15 %
  from one cell to the next, everywhere, and it changes when the map resolution or cell order changes. Key it on the
  quantized direction, or drop it, before a climate map feeds anything canonical.

## 5. Screenshots

Lab, Metal, the real-time renderer (no shadows, ADR 0040), from `veach-bench-seams shots --out build/seams-shots`.
Views rotated so the site's up is +Y (visual only). Chunks are colored by key parity, so neighboring chunks differ.

| Image | What I saw |
|---|---|
| [pole_pit.png](seams/pole_pit.png) | SP-12's pit dug at the north pole. Four chunk columns (yellow, gray, teal, green) meet on the spin axis in the pit. The ball (white) rests in the sump right on that line, and the dented mound sits at the upper left. The character (dark) stands east of the pit. Its tilt is the steep camera's perspective. No crack along any color boundary |
| [pole_overview.png](seams/pole_overview.png) | The same from a lower camera: the four columns, the pit and the character standing upright on radial up |
| [lod_plane_x.png](seams/lod_plane_x.png) | The plane_x site, 150 m up: LOD0 (red), the ring to L1 (orange), L2 (green) and L3 (blue). The right half is x < 0 (darker). The level rings run straight across the plane, and no crack shows where levels or octants change |
| [corner.png](seams/corner.png) | A character standing on the chunk corner 5 mm from the surface. Seven or eight chunk colors meet under its feet, and the surface is continuous through all of them |

## Bugs found

- **No engine bug at these places.** Floor division and rounding are right for negative coordinates in
  `engine/voxel`'s selection, sampling, mesher and edits, and in the spikes' fields.
- **Chunk collider borders bump characters** (above). This is not a bug in our code: Jolt does what it is told with
  per-chunk shapes. ADR 0058's collider layout needs the adjacency ring. It is not fixed here because the collider
  path is still spike code. It goes into `engine/physics`, recorded in VCH-113.
- **In the sweep's own harness, fixed:** the first walker timed its legs instead of ending them at a target point.
  Slopes push a little sideways and along on each leg, so after 25 to 60 legs it walked off the 80 m terrain region
  and fell through the empty space below. That happened in every collider variant, the merged control included,
  which is how I knew it was the harness. Jolt's ray test is not watertight either: a ray exactly through a shared
  vertex can miss. The height probe retries 1 mm aside.

## Decisions for the dev lead

1. **Terrain colliders get neighbor triangles as adjacency** (ADR 0058 amendment, built in VCH-113). The alternatives:
   one merged collider per region gives the same result, but every edit would recook the whole region, and
   recooking is already the largest edit cost (SP-12). Enhanced internal edge removal alone doesn't fix it.
2. **The cube-corner directions need no more testing** for this octree. The octant planes and the poles are its
   seams, and both are covered by committed tests now.

## Follow-ups

- **VCH-113 (`engine/physics`)**: adjacency rings for character contacts, and a way to ignore ring triangles for
  rigid bodies (`mIsSensor` per contact), measured with the ball rolling over chunk borders. SP-13's 13 to 14 losses
  a day should go away with the ring. Re-run its 24 h walk on the engine path to confirm.
- **The flight model** (VCH-113 and VCH-115): the craft's horizontal axes come from a transported frame, never from
  ŷ × up.
- **The climate stage** (VCH-112 and later): seamless reads, and SP-07's per-cell-index jitter keyed on position.
- **Windows CI** runs `[seams.AC1]` to `[seams.AC6]` on the next push.

## Reproduce

```sh
build/debug/tests/veach-tests -tc="*seams*"                    # the committed hashes and criteria
cmake --preset release && cmake --build --preset release --target veach-bench-seams
build/release/tests/seams/veach-bench-seams lod                # the weld table (--root-level N)
build/release/tests/seams/veach-bench-seams dig                # SP-12's flow at each site, and the control
build/release/tests/seams/veach-bench-seams walk --legs 200    # contact losses and |Δv| in the five collider variants
build/release/tests/seams/veach-bench-seams trace --legs 2 --out build/seams-trace   # every step, as CSV
build/release/tests/seams/veach-bench-seams pole               # SP-13's landing toward the pole
build/release/tests/seams/veach-bench-seams cubemap            # SP-07 and SP-06 across face edges
build/release/tests/seams/veach-bench-seams shots --out build/seams-shots
build/debug/apps/lab/veach-lab --project build/seams-shots/pole --rhi metal --headless --frames 3 \
    --script scripts/pole_pit.luau --screenshot pole_pit.png
```
