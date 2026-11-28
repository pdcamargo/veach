# 0073: The job system: enkiTS behind veach::JobSystem, fixed chunks, results in index order

Date: 2026-11-27

## Status

Accepted. Adds enkiTS to the dependency set of [0017](0017-dependency-set.md) and a `jobs` init step to the order
of [0018](0018-engine-lifecycle-and-phases.md). Implements VCH-015 ([spec](../specs/job-system/spec.md)).

## Context

The engine was single-threaded. Procgen batches ([0052](0052-procgen-runtime.md)), chunk meshing
([0053](0053-terrain-density-field.md)), physics ([0058](0058-physics-jolt.md)) and simulation LOD
([0057](0057-simulation-lod.md)) all need worker threads, and the engine owns the threads (non-negotiable 1). What
the job system must do:

- **Determinism** ([0043](0043-determinism-tiers-and-strict-floating-point.md)): canonical results never depend on
  the thread count; parallel jobs write indexed slots and reduce in index order.
- **Jolt's `JPH::JobSystem`:** research 09 plans `JobSystemWithBarrier` on our scheduler. Jolt creates jobs from
  inside jobs, and its barrier helps run them while waiting.
- **Flecs worker threads.** Flecs 4.2 has two hooks. `ecs_set_threads` starts long-lived threads through
  `ecs_os_api.thread_new_`. `ecs_set_task_threads` creates N - 1 tasks through `ecs_os_api.task_new_`/`task_join_`
  around every `ecs_run_pipeline` call (`src/addons/pipeline/worker.c`). Each task runs `flecs_worker`, which
  blocks on condition variables until all N - 1 have started, and the main thread spins until they have. So every
  task must run at the same time on its own thread: a worker busy with a long job stalls the frame, and a pool of
  fewer than N - 1 free workers deadlocks. The OS API is process-wide, set before the first world.
- Headless CI, MSVC with warnings as errors, Tracy zones on workers, ASan and `leaks` clean, and a world that only
  the main thread touches.

## Decision

**Library: enkiTS v1.12** (zlib, Doug Binks), pinned in `cmake/VeachDependencies.cmake`. Only `TaskScheduler.cpp`
is built, as `veach_enkits`, private to `veach_core`. No engine header includes it.

**API** (`veach/core/jobs.hpp`), a service created in the new `jobs` init step:

| | |
|---|---|
| `JobSystem(threads)` | `threads` counts the main thread; 1 = no workers; 0 = `default_job_threads()` (hardware threads − 1, at least 1); at most 256 |
| `parallel_for(label, count, grain, body(begin, end))` | Blocking. Chunks are `[k·grain, min(count, (k+1)·grain))`, whatever the thread count |
| `parallel_reduce(label, count, grain, init, map, combine)` | Map per chunk in parallel, then `combine` left to right in chunk order on the caller |
| `TaskGroup{jobs}.run(label, fn)`, `wait()` | Independent jobs; `run` from any job thread, also from inside a job of the group; the destructor waits |
| `wait_idle()`, `thread_count()`, `thread_index()`, `on_job_thread()` | Drain; sizes; per-thread scratch index (never for results) |

**Rules.**

1. Results never depend on the thread count. A result may depend on `grain`, which is part of the algorithm's
   definition and fixed in code, never derived from the thread count.
2. Jobs never touch the World. Inputs are prepared before the call, outputs land in indexed slots, and the main
   thread applies them. Simulation never depends on *when* a job finishes: if a result feeds simulation, the
   simulation waits for it at a fixed tick.
3. Only the main thread and job threads submit or wait (asserted). A job may run a nested `parallel_for` or
   `TaskGroup`; the waiting thread runs other jobs meanwhile.
4. Jobs do not throw. Tracy names each worker `veach worker N` and puts a `jobs` (parallel_for) or `job` (TaskGroup)
   zone around each piece of work, carrying its label.
5. **Nothing else starts threads.** `check_repo.py` fails on `std::thread`, `std::jthread`, `std::async`,
   `pthread_create`, `CreateThread`, `_beginthread` and Flecs `set_threads`/`set_task_threads` under `engine/`,
   `space/`, `apps/` and `tools/`. The only exception is the MCP server's stdin reader, which blocks in `getline`
   for the whole session and would take a worker away from jobs for good. Tests may start threads to test thread
   safety.

**Config.** `"jobs"` in `veach.project.json` and `--jobs N` (CLI wins). Default 0 (automatic). `--jobs 1` runs every
job on the main thread, in chunk order, which helps when debugging.

**Lifecycle.** `core → jobs → platform → ecs → assets → script → rhi → render → modules`. `shutdown()` runs the
Shutdown phase, calls `wait_idle()`, then unwinds the steps. Every job finishes before anything it might read is
torn down, and the workers are joined last, just before `core`.

**Consumers, planned:**

- **Jolt (VCH-087):** a `JPH::JobSystemWithBarrier` subclass in `engine/physics`. `QueueJob` becomes
  `TaskGroup::run`, one group per physics step, waited at the end of the step. Jolt's barrier runs ready jobs while
  it waits, so `--jobs 1` works.
- **Flecs (VCH-107):** `multi_threaded()` systems through `ecs_set_task_threads`. The process-wide `task_new_` hook
  finds the world's `JobSystem` through the stage (`ecs_get_world`) and runs `flecs_worker` as an enkiTS pinned task
  on its own worker. Flecs gets K ≤ worker count stages, those workers take no long jobs, and the hook is installed
  before the first world. Until then no Veach system is multi-threaded, and `set_threads` stays banned.
- **Procgen and meshing:** `parallel_for` over sample batches and chunks, results in indexed slots. The per-worker
  Luau VMs of 0052 are indexed by `thread_index()`.

**Measured** (M5 Max, 18 hardware threads, release, `veach-bench-jobs`, 10⁷ `veach::hash` items, grain 16384):

| threads | 1 | 2 | 4 | 8 | 16 | 17 (default) | 18 |
|---|---|---|---|---|---|---|---|
| ms | 63.5 | 31.8 | 16.3 | 8.6 | 4.15 | 3.94 | 3.65 |
| speedup | 1.00× | 2.00× | 3.90× | 7.38× | 15.3× | 16.1× | 17.4× |

At 10⁶ items (245 chunks) the speedup is 14.5×. A 64-chunk `parallel_for` with no work costs about 20 to 30 µs when
the workers have to wake up. The result hash is identical on every line.

## Consequences

- The four toolbox areas can start. Canonical work gets determinism across thread counts from the API itself:
  fixed chunks and reduction in chunk order.
- The whole engine sees a single, small scheduler. Priorities and pinned tasks exist in enkiTS when streaming (VCH-031) or
  Flecs (VCH-107) need them, without a new dependency.
- Fixed chunking costs some balance: with fewer chunks than threads, some threads idle. Consumers pick a grain that
  gives at least a few chunks per thread.
- enkiTS keeps its thread number in a process-wide `thread_local`. Several job systems in one process (tests) are
  fine as long as a job only uses the job system it runs on.
- New threads start with the default floating-point environment (round to nearest, no flush to zero) on macOS and
  Windows; `[jobs.AC6]` checks that workers give the main thread's bits, subnormals included. The FP self-test of
  0043 will also run in each worker's start callback when it lands.
- Revisit if Jolt or Flecs integration shows enkiTS's scheduling overhead in Tracy, or if we need job graphs with
  dependencies beyond what `TaskGroup` and nested waits give.

## Alternatives considered

- **Taskflow (MIT):** a strong task-graph executor, but header-only C++17 templates throughout (compile time in
  every translation unit that schedules work), its own executor threads with no pinning (Flecs's blocking workers
  need their own threads), and much more surface than we use. Its graph model duplicates what Jolt and Flecs
  already schedule themselves.
- **Our own pool** (a mutex-and-condition-variable queue): no dependency and full control, but we would have to
  write and maintain work stealing, help-while-waiting for nested jobs, and wake-up latency tuning. enkiTS has done
  all three for years, and its API does not leak past `jobs.cpp`, so replacing it later stays cheap.
- **Flecs's own threads (`ecs_set_threads`):** they only run ECS systems, and would be a second pool next to Jolt's
  and procgen's.
- **Jolt's `JobSystemThreadPool`:** a third pool, used only by physics.
- **Deriving the chunk size from the thread count** (what enkiTS does internally): balances load better, but
  results would then depend on the machine, which 0043 forbids.
