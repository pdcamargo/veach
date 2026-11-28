# 0075: Amendments from SP-11: Jolt's build flags, AVX2 on x64, Jolt optimized in Debug

Date: 2026-11-27

## Status

Accepted. Amends [0058](0058-physics-jolt.md). Validated by SP-11 (VCH-087,
[findings](../research/spikes/sp-11-jolt.md)).

## Context

SP-11 built Jolt v5.7.0 with `JPH_DOUBLE_PRECISION` and `JPH_CROSS_PLATFORM_DETERMINISTIC` under ADR 0043's strict
flags and dropped 200 bodies into a heightfield bowl at the origin and 6,371 km out. The state hash after every one of
600 steps, and Jolt's own `SaveState`, are equal on macOS arm64 (Debug, Release, ASan) and on x86-64 under Rosetta
with SSE2, SSE4.1 and AVX2, with 0, 1, 4 and 8 workers. Without the cross-platform flag the hash diverges by step 60.
Origin and offset differ only by the pile's chaos: a 1e-9 m nudge at the origin spreads it as much. 200 bodies cost
0.2 ms per step; 2,000 cost 2.9 ms single-threaded and 1.45 ms on 4 threads. The cost of double precision and
determinism together was below the measurement noise.

Used as a subproject, Jolt's CMake changes several things that would fight our build, and two choices were left open.

## Decision

1. **Jolt's build options are part of the dependency pin**: cross-platform deterministic and double precision on; its
   static MSVC runtime, flag overrides, LTO, warnings-as-errors, `/Zi`, floating-point exception toggling, debug
   renderer, profiler, object stream and GPU compute off (the table in the findings says why for each). A change to
   any of them is re-validated with the SP-11 hashes.
2. **x64 builds compile Jolt for AVX2** (`USE_AVX2` with `/arch:AVX2` on MSVC), set explicitly when `engine/physics`
   lands; today our Ninja + MSVC build gets SSE2 by accident. The bits are equal across SSE2, SSE4.1 and AVX2, so
   this is a speed and minimum-CPU choice; Intel and AMD desktop CPUs have had AVX2 since 2013 and 2015, older than
   any machine with the bindless GPU the platform target requires ([0003](0003-platforms-and-hardware-target.md)).
3. **Debug builds optimize Jolt** (`-O2`, keeping `JPH_DEBUG` and its asserts) on Clang. Unoptimized Jolt is about 30×
   slower, which makes physics tests dominate the fast test set and Debug play unusable. MSVC Debug stays unoptimized
   for now: `/O2` conflicts with `/RTC1`, and Windows is CI-only.
4. **Jolt's `JobSystemThreadPool` never ships**: it owns threads. `engine/physics` implements `JobSystemWithBarrier`
   over `veach::JobSystem` ([0073](0073-job-system-enkits.md)), and its integration test is the SP-11 hash on the
   engine's workers.
5. **Process-wide Jolt state** (allocator hooks, `Factory`, `RegisterTypes`) is initialized once at module start; each
   scene owns its `PhysicsSystem` and a `TempAllocator` sized for its body count.

## Consequences

- Physics replays and cross-checks bit for bit on Mac and Windows; a server and its clients agree on rigid bodies.
- Stepping into Jolt in a Clang Debug build shows optimized frames; rebuild with the line removed when that matters.
- Bit equality between two placements of the same scene is not a goal and is not tested; each placement is
  deterministic by itself, which is what replay and the platform check need.

## Alternatives considered

- **Leave the ISA to the generator:** a Visual Studio build would get AVX2 and a Ninja build SSE2, a silent speed
  difference between two Windows builds of the same commit.
- **SSE4.2 as the x64 floor:** wider hardware reach, unmeasured speed cost; nothing in the platform target needs it.
- **Fewer steps in Debug tests:** hides the problem from tests but leaves Debug physics 30× slow.
