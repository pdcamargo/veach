# SP-11: Jolt in Veach, deterministic (VCH-087)

Validates the determinism claim of [ADR 0058](../../adr/0058-physics-jolt.md): Jolt Physics v5.7.0 built with
`JPH_DOUBLE_PRECISION` and `JPH_CROSS_PLATFORM_DETERMINISTIC`, under the strict floating-point flags of
[ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md), gives the same state hash on every platform
and with any number of workers. Research: [09-physics](../09-physics.md) spike S1.

**Verdict: the decision holds**, Windows CI included. A pile of 200 boxes, spheres and capsules dropped into
a heightfield bowl gives the same state hash after every one of 600 steps on:

- macOS arm64 (NEON): Debug, Release and Debug with ASan/UBSan;
- x86-64 under Rosetta 2: SSE2 only (what MSVC x64 compiles under our Ninja build), SSE4.1 and AVX2, in Debug and
  Release;
- `JobSystemSingleThreaded`, and `JobSystemThreadPool` with 1, 4 and 8 threads;
- both scene positions: at the origin, and 6,371 km out. Each position has its own hash.

Jolt's own `SaveState` output is byte-identical too. The hashes are committed in `[sp11.AC1]` and `[sp11.AC2]`, and
Windows CI checks them on the next push. Built without `CROSS_PLATFORM_DETERMINISTIC`, the same scene on the same Mac
diverges by step 60, so the test can detect a broken build.

The two positions give different hashes, as they must. Placing a body 6,371 km out rounds its position by up to
9.3e-10 m, and the pile is chaotic: moving every body 1e-9 m at the origin gives the same spread (about 0.5 m mean,
4 m max after 10 s). No large-world defect showed between 1 km and 10,000 km.

## What was built

Throwaway, under `tests/sp11/`, like SP-06. Nothing was added to `engine/`: `engine/physics` should start from
ADR 0058's scene design and the VCH-015 job system, not from this test scene.

| Piece | Where | Notes |
|---|---|---|
| Jolt v5.7.0, pinned | `cmake/VeachDependencies.cmake` | Release tarball, SHA256, CPM `SOURCE_SUBDIR Build`, `EXCLUDE_FROM_ALL`; MIT, in `THIRD_PARTY_NOTICES.md` |
| Scene and hash | `tests/sp11/sp11_scene.cpp` (`veach-sp11` library) | The only file that includes Jolt. Jolt links `PRIVATE`; `sp11_scene.hpp` has no Jolt types |
| Tests | `tests/sp11/sp11_jolt_tests.cpp` | In `veach-tests`, the fast set: about 7 s each in Debug on an M5 Max, 17 s under ASan |
| `veach-bench-sp11` | `tests/sp11/sp11_jolt_bench.cpp` | The tables below. `--sweep [--mesh] [--boxes] [--bodies N]` is the large-world sweep |
| wincheck | `tools/scripts/wincheck.sh` | Adds Jolt's headers and its two defines |

**The scene.** The ground is a static paraboloid bowl, 64 × 64 samples 1 m apart and 15.5 m deep at the corners, as
a `HeightFieldShape` (the tests) or the same grid as a `MeshShape` (`--mesh`). The 200 dynamic bodies are boxes,
spheres and capsules in turn, in layers of 6 × 6, 1.5 m apart, from 4 m up, with jittered positions, rotations and
spin. The 2,000-body run uses layers of 15 × 15. Every input is built from integers with `+ − × ÷ √`, so the inputs
have the same bits everywhere and only Jolt's arithmetic is under test. As in ADR 0058, world gravity is zero and
each awake body gets a force of m·g before each step. Each run is 600 steps of 1/60 s with one collision step. The
offset scene puts the ground and every body at (3,822,600, 5,096,800, 0) m, 6,371 km out along a 3-4-5 direction, so
two axes are large.

**The hash.** After every step it covers each dynamic body, in creation order: position relative to the scene origin
(three doubles), rotation (four floats), linear and angular velocity (three floats each), all as raw bits. The tests
check the hash after steps 1, 60, 300 and 600, plus a chain of all 600. The N-worker run must match the 1-worker run
at every step and in Jolt's `SaveState` (bodies, contact cache, warm-start impulses).

## Build flags

Jolt's `Build/CMakeLists.txt` is written to be the top-level project. Used as a subproject, it changes these things,
and we set:

| Jolt default | Effect on us | Set |
|---|---|---|
| `CROSS_PLATFORM_DETERMINISTIC` adds `-ffp-contract=off` (Clang) or `/fp:precise` (MSVC). Without it: `-ffp-contract=fast` on x86, `=on` on arm64, `/fp:fast` on MSVC | **The one flag that matters.** The deterministic branch matches ADR 0043 and the fast branch is never taken. It also stops `JPH_USE_FMADD`, Jolt's explicit FMA intrinsics | `ON` |
| `USE_STATIC_MSVC_RUNTIME_LIBRARY=ON` sets `CMAKE_MSVC_RUNTIME_LIBRARY` to `/MT` | LNK2038 runtime mismatch with our `/MD` | `OFF` |
| `OVERRIDE_CXX_FLAGS=ON` adds `/GS- /Gy /Oi /Ot` to MSVC Release | Not FP related, but not ours | `OFF` |
| `INTERPROCEDURAL_OPTIMIZATION=ON`: LTO in Release | `/GL` objects in a library our build doesn't link with `/LTCG` | `OFF` |
| `ENABLE_ALL_WARNINGS=ON`: `-Wall -Werror`, `/Wall /WX` | A Jolt warning on a new compiler would break our build | `OFF` (Jolt is `SYSTEM`) |
| `GENERATE_DEBUG_SYMBOLS=ON`: `/Zi`, `-g` | `/Zi` conflicts with our embedded `/Z7` (ccache) | `OFF` (Debug keeps `-g` and `/Z7`) |
| `FLOATING_POINT_EXCEPTIONS_ENABLED=ON` (MSVC Debug and Release) | Jolt flips the MXCSR exception masks around some code; ADR 0043 asserts the FP environment | `OFF` |
| Debug renderer, profiler, ObjectStream, DX12/Vulkan/Metal compute, install | Unused; GPU compute would pull in shader compilers | `OFF` |
| `CPP_RTTI_ENABLED`, `CPP_EXCEPTIONS_ENABLED` | Jolt's `-fno-rtti -fno-exceptions` (and `/GR-`, `/EHsc` removed) apply only to Jolt's directory | Jolt's defaults (`OFF`) |
| `USE_AVX2` and friends: `/arch:AVX2` on MSVC **only with a Visual Studio generator** (`CMAKE_VS_PLATFORM_NAME`), `-mavx2 …` on x86 Clang | Our Ninja build passes no ISA flag, so MSVC x64 builds Jolt for **SSE2**. Jolt detects the ISA from predefined macros, so the library and its users always agree | Untouched; see follow-ups |

Nothing Jolt sets conflicts with ADR 0043 in deterministic mode. Jolt's own `-ffp-contract=off` comes after our global
flag and repeats it. `check_repo.py` lints only our build files, and Jolt's fast-math lines sit in branches we never
take. Jolt also builds as C++17 (its own `CMAKE_CXX_STANDARD`), and that is harmless.

Jolt's `PUBLIC` compile definitions (`JPH_DOUBLE_PRECISION`, `JPH_CROSS_PLATFORM_DETERMINISTIC`,
`JPH_OBJECT_LAYER_BITS=16`, `JPH_DEBUG` in Debug) reach `veach-sp11` only. `RegisterTypes()` aborts on a mismatch
(`JPH_VERSION_ID` encodes them). Jolt itself never sets FTZ/DAZ: `FPFlushDenormals` is defined but unused.

## Hashes

| Scene | Step 1 | Step 60 | Step 300 | Step 600 | Chain (600 steps) | Jolt `SaveState` |
|---|---|---|---|---|---|---|
| 200 bodies, origin | `0x90f546172584bb1f` | `0x89a2820b9fb3be9e` | `0x79d47a6ec4279409` | `0x87bdb4156d528362` | `0x171ce203da644de9` | `0xb82ff6c6c04b4f7d` |
| 200 bodies, 6,371 km | `0x82536d1fc1f7bec7` | `0xd756aa6a7898d1f0` | `0x8e0eb4048226ac7f` | `0x364bc02eeecdeb99` | `0x48bceab5bca8cea8` | `0x7ddfc5ce2849c36a` |
| 2,000 bodies, origin (bench) | | | | `0xde4c302b4d9ba28a` | `0x7a8299d85e33f25d` | `0x6c70172a57c1a478` |
| 2,000 bodies, 6,371 km (bench) | | | | `0x8a5f53e59507855f` | `0x30a51875859dd937` | `0x511d745f74fcefe6` |
| Control: 200, origin, arm64 **without** `CROSS_PLATFORM_DETERMINISTIC` | `0x90f546172584bb1f` | `0xcb7e14a19f6a61e7` | `0xc8df09993a390358` | `0x24d309b0b66ddd21` | `0x27e61acc99ed8ec4` | `0x07053da10ce1733c` |

Step 1 is free fall, so it matches even in the control. The control diverges once bodies touch.

| Build | 200-body hashes (both scenes) |
|---|---|
| macOS arm64, Apple Clang 21, Debug (`veach-tests`, the pre-push gate) | committed values |
| Same, Release | equal |
| Same, Debug + ASan/UBSan (the CI preset) | equal |
| x86-64 under Rosetta 2, SSE4.1 (Clang's default), Release, scratch harness | equal |
| Same, SSE2 only (`-mno-sse3 … -mno-sse4.2`, what MSVC x64 gets), Debug | equal |
| Same, AVX2 + BMI + LZCNT + F16C (Jolt's x86 default), Release | equal |
| 0, 1, 4 and 8 workers, each of the above that ran the bench or harness | equal |
| **Windows x64, MSVC, Debug (CI)** | **equal** (CI run 46087974524, commit 79db0bb) |

The x86-64 harness was a scratch CMake project, not committed. It built Jolt from the pinned cache with the same
options and `-ffp-contract=off -fno-fast-math`, and linked `sp11_scene.cpp`.

## Large worlds: origin vs 6,371 km

ADR 0058 keeps each scene near its own origin. This checks what Jolt does when it isn't. `veach-bench-sp11 --sweep`
runs the 200-body scene at growing distances. It compares the final relative positions with the origin run, then
with the origin run where every body starts 1e-12, 1e-9 or 1e-6 m to the side (Release, 1 worker, 10 s simulated):

| Run | Mixed shapes: awake at end | Drift: mean / max m | Boxes only: awake | Drift: mean / max m |
|---|---|---|---|---|
| origin | 2 | 0 | 0 | 0 |
| 1 km | 2 | 6.3e-08 / 7.8e-07 | 0 | 3.4e-12 / 2.8e-10 |
| 10 km | 177 | 0.31 / 3.7 | 0 | 0.18 / 3.0 |
| 100 km | 183 | 0.25 / 2.5 | 0 | 0.40 / 2.9 |
| 1,000 km | 182 | 0.44 / 3.5 | 0 | 0.53 / 3.8 |
| 6,371 km | 183 | 0.51 / 3.4 | 0 | 0.59 / 4.6 |
| 10,000 km | 188 | 0.45 / 2.9 | 0 | 0.63 / 4.6 |
| origin, bodies moved 1e-12 m | 2 | 1e-12 / 1e-12 | 0 | 0.005 / 0.32 |
| origin, bodies moved 1e-9 m | 179 | 0.26 / 3.7 | 0 | 0.48 / 4.3 |
| origin, bodies moved 1e-6 m | 185 | 0.54 / 3.5 | 0 | 0.67 / 4.6 |

- **The divergence is chaos, not precision.** A 1e-9 m nudge at the origin spreads the pile as much as moving it
  10,000 km away. 200 boxes come to rest at every distance (the fastest moves under 1e-4 m/s at the end), so
  nothing jitters.
- **Sleep depends on chance, not on distance.** In the mixed pile, one sphere still rolling in the bowl keeps its
  whole island awake: 2 or 183 bodies. With 2,000 boxes the origin run ends with 1,956 awake and the 10 km run with 0.
  The mesh ground behaves the same (`--mesh`).
- **Bit equality between two positions is not a goal.** Each position is deterministic by itself. That is what replay
  and the cross-platform check need.

## Timings

`veach-bench-sp11`, Release, Apple M5 Max (6 performance and 12 efficiency cores). Other worktrees were building at
the same time (load average 13), so the per-step times are upper bounds. Mean over 600 steps, then p95, in ms:

| Bodies | Scene | 0 workers (single-threaded) | 1 thread | 4 threads | 8 threads |
|---|---|---|---|---|---|
| 200 | origin | 0.19 / 0.33 | 0.19 / 0.26 | 0.17 / 0.33 | 0.26 / 0.38 |
| 200 | 6,371 km | 0.22 / 0.36 | 0.21 / 0.27 | 0.17 / 0.27 | 0.29 / 0.41 |
| 2,000 | origin | 2.91 / 4.42 | 2.18 / 2.90 | 1.45 / 2.23 | 1.65 / 2.29 |
| 2,000 | 6,371 km | 3.59 / 5.09 | 2.50 / 3.24 | 1.61 / 2.32 | 1.86 / 2.43 |

- 200 bodies cost 0.2 ms per step whatever the worker count. Threads only pay off for large piles: 2,000 bodies scale
  2× on 4 threads, and 8 threads are slower than 4 (probably job overhead and the efficiency cores).
- The 6,371 km runs cost 10-20 % more for 2,000 bodies. Their piles stayed awake (1,986 bodies) while the origin pile
  fell asleep, so the gap is the chaos above and not the distance.
- **The cost of the two modes is too small to measure here.** Best-of-3 single-threaded means for 2,000 bodies, in
  four scratch builds: deterministic double 3.32 ms, double alone 3.16, deterministic float 3.03, float alone 3.47.
  Run-to-run noise on this machine is larger than the 5-10 % and 8 % that Jolt quotes.
- Debug is about 30× slower than Release (unoptimized Jolt with asserts). The two test cases take about 7 s each.

## Follow-ups

- **Windows CI** runs `[sp11.AC1]` and `[sp11.AC2]` on the next push. VCH-087 is done when they pass there.
- **Job system (VCH-015).** ADR 0058's plan stands: `JobSystemWithBarrier` over the engine's job system. The hash
  doesn't depend on the job system (single-threaded and 1, 4 and 8 pool threads agree), so the integration test is
  this same check on the engine's workers. Jolt's `JobSystemThreadPool` makes its own threads and must not ship:
  "never own the loop" applies.
- **Scene per frame.** One `PhysicsSystem` per scene fits Jolt as ADR 0058 assumes (Jolt shapes are refcounted and
  can be shared between systems; not exercised here, each run builds one system). The allocator hooks, `Factory::sInstance` and `RegisterTypes()` are
  process-wide, so `engine/physics` initializes them once at module start, not per scene. Each scene needs its own
  `TempAllocator`, sized for its body count (64 MB was plenty for 2,000 bodies).
- **Gravity registry.** Adding m·g only to awake bodies is deterministic and lets piles sleep. Calling `AddForce` with
  `EActivation::Activate` would keep every body awake forever.
- **Terrain colliders (SP-12, VCH-088).** `MeshShape` and `HeightFieldShape` are both deterministic here, at both
  positions. For collider installs, the hash of an install has to come from canonical inputs. Here the inputs are
  integer-built floats. SP-12's meshes come from the canonical density and must be bit-equal before cooking.
  `HeightFieldShape` quantizes samples (8 bits per block by default); that is deterministic but lossy.
- **Contact order.** Not exercised (no listener). ADR 0058's sort by (body, body, sub-shape) still applies when
  gameplay sees contacts.
- **x64 instruction set.** Our Ninja + MSVC build compiles Jolt for SSE2. A Visual Studio generator would compile it
  for AVX2, and so would the Clang flags on x86 Linux. The results are bit-equal either way (Rosetta, above), so this
  is a performance choice and a minimum-CPU choice, not a determinism one. Set it explicitly (`USE_AVX2` with
  `/arch:AVX2`) when `engine/physics` lands, and measure on Windows CI.
- **Debug cost.** Jolt at `-O0` makes Debug physics about 30× slower. The engine may want Jolt optimized in Debug
  builds (for example `-O2`, keeping `JPH_DEBUG`). The hashes don't change with optimization level.
- **Tracy.** S1 asked for a Tracy zone around the step. Jolt's profiler is off here; when `engine/physics` lands, wrap
  `PhysicsSystem::Update` in a `VEACH_ZONE` (or route `JPH_EXTERNAL_PROFILE` to Tracy in the profile preset).

## Reproduce

```sh
build/debug/tests/veach-tests -tc="*sp11*"                       # the committed hashes, 1 and 4 workers
cmake --preset release && cmake --build --preset release --target veach-bench-sp11
build/release/tests/sp11/veach-bench-sp11                        # hashes and timings, 200 and 2,000 bodies, 0/1/4/8 workers
build/release/tests/sp11/veach-bench-sp11 --sweep [--boxes] [--mesh] [--bodies 2000]   # large-world sweep
```
