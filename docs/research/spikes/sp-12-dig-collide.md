# SP-12: Dig and collide (VCH-088)

Validates the edit half of [ADR 0054](../../adr/0054-terrain-edits-and-water.md) (brush ops applied in integer
arithmetic to LOD0 samples, absolute overrides) and the terrain-collider half of
[ADR 0058](../../adr/0058-physics-jolt.md) (static `MeshShape` per LOD0 chunk, cooked on workers, installed at a
deterministic tick), on the mesher of [ADR 0074](../../adr/0074-amendments-from-sp-08.md) and the Jolt build of
[ADR 0075](../../adr/0075-amendments-from-sp-11.md), at a real planet-surface location
([ADR 0045](../../adr/0045-large-world-positions-and-reference-frames.md)). Research:
[04-terrain](../04-terrain.md) spikes S4.3 and S4.4.

**Verdict: both decisions hold.**

- **Same hashes everywhere.** A six-op edit log on an Earth-sized planet, 6,371 km from its center, replays to the same
  hashes in every configuration tried: macOS arm64 (Debug and Release), x86-64 under Rosetta 2 (Release with Clang's
  default instruction set, and Debug with SSE2 only, which is what MSVC x64 compiles), 1 and 4 workers, and each op
  applied to its chunks in reverse key order. The hashes cover samples, meshes, cooked Jolt shapes, the collider install and the
  ball's state after each of 1,200 steps. They are committed in `[sp12.AC2]` to `[sp12.AC4]`, and Windows CI checks
  them on the next push. The fixed-point edit path is not broken.
- **The ball falls in.** It is dropped on the dig site and is rolling about when a 3 m pit is dug under it at tick 90.
  It falls in and stops 2.59 m below the original surface, then goes to sleep. At tick 600 a sump is dug under the
  sleeping ball. The install wakes it, and it comes to rest 3.98 m down.
- **Cooking keeps up.** A 2,088-triangle chunk cooks in 0.46 to 0.65 ms on one core. That is about 8× its meshing time
  and a quarter of its sampling time. Digging a trench one brush per 60 Hz tick costs 0.84 ms from op to cooked
  colliders with a 1.5 m brush, and 1.10 ms with a 4 m brush, on 4 workers: 5 to 7 % of a tick. A brush at the 16 m
  cap, every tick, costs 4.5 ms (27 %). The SDF-collider fallback of ADR 0058 is not needed.

The spike also found four things the ADRs should say (under "Proposed amendments"): the brush's reach, chunks
materialized on first touch, waking bodies when a collider is swapped, and a value for K.

## What was built

Throwaway, except the edit record and its applier, which ADR 0054 puts in `engine/voxel` and SP-20 (VCH-096) will
need as they are.

| Piece | Where | Notes |
|---|---|---|
| `BrushOp`, `apply_brush`, `touched_chunks`, `samples_hash`, `isqrt` | `engine/voxel/include/veach/voxel/edit.hpp`, `src/edit.cpp` | Canonical, integers only. `add` and `subtract` spheres. `ChunkSamples` gained a mutable `at()` |
| Planet field and terrain region | `tests/sp12/sp12_terrain.*` (`veach-sp12` library) | Earth radius, three octaves of `veach::hash` value noise (±7.85 m), regolith over rock. Holds (2h + 1)³ LOD0 chunks around the site and applies ops on `veach::JobSystem` |
| Colliders and physics scene | `tests/sp12/sp12_physics.*` | The only SP-12 file that includes Jolt; Jolt links `PRIVATE`. Shares SP-11's process-wide Jolt init (`sp11::ensure_jolt`) because both run in `veach-tests` |
| The scenario | `tests/sp12/sp12_scenario.*` | Load, cook, install, drop the ball, apply the log at its ticks, recook, install at t + K |
| Tests `[sp12.AC1]`..`[sp12.AC4]` | `tests/sp12/sp12_tests.cpp` | In `veach-tests`, the fast set. AC2 to AC4 are subcases of one test case, so the four scenario runs they share happen once per process: about 8 s in Debug on an M5 Max |
| `veach-bench-sp12` | `tests/sp12/sp12_bench.cpp` | The tables below; `--out DIR` writes a lab project for screenshots; `--surface E N` and `--basins` survey the ground |

Tracy zones (`sp12 sample chunk`, `sp12 apply brush`, `sp12 mesh chunk`, `sp12 cook collider`,
`sp12 install colliders`, `sp12 physics step`) are in place for a profile build. The numbers below come from the
bench's steady-clock timings.

### The edit record and its arithmetic

`BrushOp { version, kind, shape, center_mm (int64 ×3, body-local), radius_mm, material, tick, seq }`, about 49 bytes.
Orientation, strength, falloff and author (ADR 0054) arrive with the kinds that read them. Applying an op to one LOD0
chunk:

1. For each grid sample (apron included, −1..33 per axis) inside the brush's reach (radius + 2 LOD0 cells),
   `d² = Σ (p·500 − c)²` in mm², from the sample's lattice point `p` and the center `c`.
2. The brush's signed distance in 1/1024 m: `round((isqrt(d² · 2²⁰) − 1024 · r) / 1000)`. `isqrt` is the exact
   bit-by-bit integer square root, and the rounding is floor division, the same on every compiler.
3. `subtract`: `max(old, −brush)`. `add`: `min(old, brush)`, and the op's material wherever the brush wins. The result
   is clamped to ±32767, and the mask is set where the sample changed.

Each chunk is independent. The new value depends only on the old value at the same lattice point, so neighbors stay
equal at shared positions; the scenario checks this after every run (`mismatches == 0`). The ops of one tick run in
`seq` order, and the chunks of one op in parallel on `veach::JobSystem`. The changed chunks are remeshed in the same
job. `add` and `subtract` don't commute, and `[sp12.AC1]` checks that order changes the result.

### The scene

- **Where.** The dig site is a basin on the surface along (0.6, 0.8, 0) from the planet's center, at
  (3,822,603.093, 5,096,792.457, 0) m, so two axes are large. A ball comes to rest in a basin. It never does on
  the slopes around the first site I tried: it rolled 6 m away within 3 s. The region is 5³ LOD0 chunks (80 m)
  around the site's chunk: 78 sampled, 51 with surface. The tests use this region; the timing tables use 7³ chunks.
- **The log.** Tick 90: a 3 m pit centered on the surface; a 2 m sphere deepening it off-center; a 1.5 m mound of
  material 3 five meters west, with a 0.8 m dent cut into its top; and a 2.5 m cavity 25 m down, in chunks that the
  bounds had proven solid, so they had never been sampled. Tick 600: a 1.2 m sump under the pit's bottom.
- **Physics** ([ADR 0045](../../adr/0045-large-world-positions-and-reference-frames.md), 0058). One scene whose
  origin is the dig-site chunk's minimum corner, a LOD0 lattice point. Its axes are the body-fixed axes, only
  translated. Each chunk is a static body at `(chunk origin − scene origin) × 0.5 m`, an exact double. Its
  `MeshShape` vertices are the mesher's chunk-relative floats, multiples of 2⁻¹⁷ m under 16 m, also exact. So the
  collider is a pure function of the canonical mesh, with no rounding between them. World gravity is zero. Before
  each step the ball gets m·g toward the planet's center, computed from its double position. The ball is a 0.4 m
  sphere with linear damping 0.2 and angular damping 1.0: with Jolt's default 0.05 it orbits the pit's floor for
  tens of seconds.
- **Install.** Colliders requested at tick t (load, or an op) are cooked at once on the workers and installed before
  step t + K, with K = 2, in chunk-key order. A new chunk becomes a new static body. A changed chunk keeps its body
  and gets the new shape through `BodyInterface::SetShape`, so body ids stay stable. An emptied chunk's body is
  removed. Every change then calls `ActivateBodiesInAABox` over the chunk, grown by 1 m.

## Hashes

From macOS arm64, committed in `tests/sp12/sp12_tests.cpp`:

| What | Hash |
|---|---|
| Base samples, the 78 sampled chunks before any edit (ADR 0054's `base_hash`, folded) | `0xace58c96ce261f0d` |
| Base meshes | `0x796b6be1210887a9` |
| Samples after the log (81 chunks: 3 materialized by the cavity) | `0xd0163ab3d7069434` |
| Meshes after the log (lattice-coordinate positions, ADR 0074) | `0x1be19fd52b71cdad` |
| Every cooked `MeshShape` after the log (Jolt's `SaveBinaryState`, folded in key order) | `0x8927396d27667325` |
| The install: per terrain body, key, body id and sequence, position bits, shape hash | `0x8097f58466b787c5` |
| Ball state after steps 60 / 120 / 600 / 1,200 | `0xd0ea06ffda4a1993` / `0x14904caff3ab6ea5` / `0x5b7f8941f7bd71b4` / `0x2fe0ad520bcd7587` |
| All 1,200 ball hashes chained | `0x25dd581926b6d60f` |
| Jolt `SaveState` after step 1,200 (compared run to run, not committed) | `0xa0735d053d797cb4` |

| Build | All of the above |
|---|---|
| macOS arm64, Apple Clang, Debug (`veach-tests`, the pre-push gate): 1 worker, 4 workers, 4 workers in reverse chunk order | committed values |
| Same, Release (`veach-tests` and `veach-bench-sp12`) | equal |
| x86-64 under Rosetta 2, Release, Clang's default instruction set (scratch build of the repo with `CMAKE_OSX_ARCHITECTURES=x86_64`), 1 and 4 workers | equal |
| x86-64 under Rosetta 2, Debug, SSE2 only (`-mno-sse3 … -mno-avx`, what MSVC x64 gets) | equal |
| **Windows x64, MSVC, Debug (CI)** | **pending: the next push** |

Other checks in every run: the dug terrain is watertight under SP-08's weld check (0 open edges in 98,584 triangles).
98,582 of those triangles agree in winding with their gradient normals; the other 2 are folds at thin features, as
in SP-08. No op reaches outside the region.

## Timings

`veach-bench-sp12`, Release, Apple M5 Max (6 performance and 12 efficiency cores). Other worktrees were building at the
same time (load average 8 to 15), so treat these as upper bounds. A second run at load 15.7 measured up to 50 % more;
the conclusions are the same. Per-chunk values are mean / median / p95 / max, in ms.

**Streaming in a region of 7³ chunks** (147 sampled, 89 with surface, 2,088 triangles per chunk on average); median of 3:

| Workers | Sample | Mesh | Cook | Load wall (sample + mesh) | Cook wall |
|---|---|---|---|---|---|
| 1 | 1.747 / 1.737 / 1.839 / 2.499 | 0.047 / 0.059 / 0.100 / 0.158 | 0.463 / 0.472 / 0.823 / 0.978 | 263 ms | 41.0 ms |
| 4 | 1.943 / 1.809 / 2.751 / 3.355 | 0.052 / 0.064 / 0.110 / 0.238 | 0.496 / 0.510 / 0.926 / 1.239 | 76 ms | 12.8 ms |

**Digging a trench**: one subtract op per tick, the center 0.5 m under the surface and moving 50 mm per op (3 m/s).
240 ops, 30 for the 16 m brush. Latency runs from the op until every changed chunk's collider is cooked:

| Radius | Workers | Chunks per op (mean / max) | Edit per chunk | Mesh per chunk | Cook per chunk | Op to colliders | Mean share of a 16.7 ms tick |
|---|---|---|---|---|---|---|---|
| 1.5 m | 1 | 2.30 / 4 | 0.021 / 0.021 / 0.023 / 0.048 | 0.077 / 0.078 / 0.086 / 0.163 | 0.630 / 0.667 / 0.742 / 0.868 | 1.68 / 1.57 / 2.40 / 2.84 | 10 % |
| 1.5 m | 4 | 2.30 / 4 | 0.023 / 0.023 / 0.028 / 0.085 | 0.081 / 0.079 / 0.092 / 0.169 | 0.651 / 0.678 / 0.772 / 1.094 | 0.84 / 0.83 / 0.95 / 1.29 | 5 % |
| 4 m | 1 | 3.27 / 8 | 0.047 / 0.058 / 0.066 / 0.122 | 0.072 / 0.083 / 0.093 / 0.220 | 0.608 / 0.777 / 0.855 / 1.062 | 2.39 / 1.95 / 4.17 / 4.69 | 14 % |
| 4 m | 4 | 3.27 / 8 | 0.054 / 0.063 / 0.078 / 0.170 | 0.079 / 0.086 / 0.112 / 0.199 | 0.649 / 0.799 / 0.921 / 1.248 | 1.10 / 1.03 / 1.55 / 1.88 | 7 % |
| 16 m (cap) | 1 | 17.1 / 24 | 0.239 / 0.169 / 0.747 / 0.828 | 0.066 / 0.073 / 0.088 / 0.143 | 0.445 / 0.520 / 0.807 / 1.074 | 13.4 / 13.1 / 13.7 / 24.2 | 80 % |
| 16 m (cap) | 4 | 17.1 / 24 | 0.265 / 0.189 / 0.770 / 1.163 | 0.074 / 0.076 / 0.120 / 0.279 | 0.490 / 0.535 / 0.942 / 2.277 | 4.49 / 4.32 / 4.72 / 9.72 | 27 % |

- **Cooking is the biggest cost of an edit**: about 8× meshing. Applying the brush is negligible. Sampling costs
  nothing on an edit, because the chunks near the player keep their samples (ADR 0053). The exception is a
  materialized chunk, which costs one sampling pass (about 1.8 ms) on first touch.
- **Parallelism is per chunk.** A hand-sized brush touches 2 to 4 chunks, so 4 workers halve the latency rather than
  quartering it. Splitting one chunk's cook isn't worth it at these sizes.
- **Streaming is not limited by cooking.** 89 colliders cook in 12.8 ms on 4 workers, about 7,000 chunks per second.
  ADR 0053's budget is B ≈ 300 chunks/s, and colliders exist only near dynamic bodies.
- **`FavorBuildSpeed`** cooks about 25 % faster: 0.39 ms per chunk on 1 worker, against 0.54 ms in a back-to-back
  run (`--build-speed`). That is not needed here; I kept Jolt's default, `FavorRuntimePerformance`.
- **Override records at rest.** These are owned samples only, at 3 bytes each before RLE and zstd. After the 1.5 m
  trench they hold 1,528 samples, 4 KB, against 11.5 KB of ops. After the 4 m trench: 24 KB against 11.5 KB. After 30
  ops at the 16 m cap, moving 1.5 m in all: 129,000 samples, 378 KB, against 1.4 KB. Overrides grow with the edited
  volume and ops with the edit count, so large brushes favor ops and long digging in one place favors overrides.
  That is not ADR 0054's reopen case ("far larger in heavy play"), but cap-size ops writing hundreds of KB of
  overrides are a cost to keep in mind when the owner sets the cap.

## Screenshots

Lab, Metal, real-time renderer, from `veach-bench-sp12 --out build/sp12`, rotated so radial up is +Y (visual only).
Colors: regolith beige, rock grey, the mound green, the ball red.

| Image | What I saw |
|---|---|
| [pit.png](sp-12/pit.png) | From 6.5 m above the pit. The 3 m pit, with rock exposed on its floor. The sump in the floor, and the ball resting in it. The mound with its dented top at the upper left. The dark band along the near rim is the steep wall in shadow, lit only by the sky: the weld check reports no holes, and with the sun overhead the dark areas moved to other walls. |
| [overview.png](sp-12/overview.png) | From 25 m: the pit and the mound on the noise terrain, with no seams at chunk borders. |

## Proposed amendments

The dev lead writes the amendment ADR; these are what SP-12 measured.

1. **ADR 0054: a brush reaches its radius plus 2 LOD0 cells, and no further.** The samples are a ±32 m narrow band,
   so a true CSG `subtract` would rewrite every sample within 32 m of the brush. Rewriting out to 2 cells past the
   surface keeps the result exact wherever the mesher reads it. The mesher reads gradients one sample past each
   crossing edge. Beyond the reach, samples keep their old value with the same sign. The reach and the integer
   formula above are part of each kind's canonical definition (`kind@version`): changing either changes every chunk
   hash.
2. **ADR 0054: override records store the owned samples.** A record holds lattice points 0..31 on each axis,
   with the mask. In memory each chunk holds its apron too, and every op writes it, but on load the apron comes
   from the neighbors' records. That stores each lattice point exactly once, and 31 % fewer samples than the 35³
   grid. Each chunk's hash covers the 35³ grid it meshes from.
3. **ADR 0053: a chunk the bounds proved uniform is sampled when an edit touches it.** It is materialized before
   the op applies: one sampling pass, about 1.8 ms. "Never sampled" holds only until the first edit.
4. **ADR 0058: K = 2.** Install colliders requested at tick t before step t + 2. A hand-sized op is cooked within
   about 1 ms on 4 workers of the M5 Max, and a 16 m op within 5 to 10 ms. K = 2 leaves 33 ms, which covers a
   cap-size op on an M1-class machine (about 2× slower per core, unmeasured) with margin. K = 1 would need every op
   cooked within the tick it was committed in, on every machine. That doesn't hold for the cap on an M1.
5. **ADR 0058: swapping a chunk's collider wakes the bodies over it.** Jolt doesn't wake sleeping bodies when a
   static body's shape changes. With the `ActivateBodiesInAABox` call removed, the ball stays asleep, hovering
   above the new sump. Replace shapes in place (`SetShape`) so body ids stay stable across edits, and the install
   hash with them.
6. **ADR 0058 (scene frames): a surface scene's origin is a LOD0 lattice point and its axes are the body's.**
   Chunk bodies then sit at exact offsets, and their meshes convert to collider vertices with no rounding. A scene
   rotated to local up would round every vertex. That is deterministic, but it is no longer the canonical mesh.

No accepted decision turned out wrong. ADR 0058's "reopen if cooking can't keep up on 4 workers" doesn't trigger.
ADR 0054's "reopen if canonical base generation proves unreproducible" doesn't either: the base hash is the same on
arm64 and x86-64.

## Follow-ups

- **Windows CI** runs `[sp12.AC2]` to `[sp12.AC4]` on the next push. VCH-088 is done when they pass there.
- **`engine/physics`** (after VCH-015): this collider path (cook on workers, install at t + K in key order, `SetShape`
  plus wake) goes there with ADR 0075's `JobSystemWithBarrier`. Jolt's process-wide init is shared by SP-11 and SP-12
  through `sp11::ensure_jolt`; the engine does it once at module start.
- **The rest of ADR 0054** is not built: the other kinds (flatten, smooth, raise, lower, paint, with orientation,
  strength and falloff), coarse-level propagation, override records on disk (RLE + zstd), and the journal. SP-20
  (VCH-096) needs the op record on the wire as it is here.
- **Measure on an M1-class machine** (SP-21, VCH-097): the M1 numbers behind K = 2 are an estimate.
- **Contact ordering** (ADR 0058's sort by body, body, sub-shape) is not exercised: there is no contact listener.

## Reproduce

```sh
build/debug/tests/veach-tests -tc="*sp12*"                     # the committed hashes, 1 and 4 workers, reverse order
cmake --preset release && cmake --build --preset release --target veach-bench-sp12
build/release/tests/sp12/veach-bench-sp12                      # hashes on 1 and 4 workers, the tables above
build/release/tests/sp12/veach-bench-sp12 --build-speed        # the same with FavorBuildSpeed
build/release/tests/sp12/veach-bench-sp12 --out build/sp12      # lab project: pit.luau, side.luau, overview.luau
build/debug/apps/lab/veach-lab --project build/sp12 --rhi metal --headless --frames 3 --script scripts/pit.luau \
    --screenshot pit.png
# x86-64 under Rosetta 2 (scratch build, not a preset):
cmake -S . -B build/x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64
cmake --build build/x64 --target veach-bench-sp12 && build/x64/tests/sp12/veach-bench-sp12 --out build/sp12x64
```
