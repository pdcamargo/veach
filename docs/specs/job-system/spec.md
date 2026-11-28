# Job system

**Status:** Implemented
**Area:** core
**Owner:** dev lead

## Summary

`veach::JobSystem` runs engine work on worker threads the engine owns: parallel loops over index ranges, map-reduce
in index order, and groups of independent jobs. Results are the same for any number of threads.
([ADR 0073](../../adr/0073-job-system-enkits.md))

## Motivation

Procgen, terrain meshing, physics and simulation LOD all need more than one core, and the engine owns every thread
(non-negotiable 1). Canonical results must not depend on the machine ([ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md)).
Backlog: VCH-015.

## Requirements

- MUST be created by the engine in the `jobs` init step, right after `core`, and provided as a service.
- MUST size itself from `jobs` in `veach.project.json` and `--jobs N` (CLI wins): 0 means the hardware threads
  minus one (at least 1), 1 means no workers, at most 256.
- MUST cut `parallel_for` and `parallel_reduce` into chunks of `grain` items that do not depend on the thread count,
  and combine reduction results in chunk order on the calling thread.
- MUST let jobs run nested `parallel_for` calls and task groups, and let jobs add jobs to a group being waited on.
- MUST finish every job before the engine tears down the world, and join the workers after every other subsystem.
- MUST NOT let anything under `engine/`, `space/`, `apps/` or `tools/` start threads other than through the job
  system, except the MCP server's stdin reader (`check_repo.py`).
- MUST name each worker and zone each job in Tracy builds.
- SHOULD scale close to linearly on the canonical workload up to the default thread count.

## API sketch

```cpp
veach::JobSystem& jobs = veach::services(world).get<veach::JobSystem>();

jobs.parallel_for("mesh chunks", chunks.size(), 1, [&](std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) meshes[i] = mesh(chunks[i]);    // indexed slots
});

std::uint64_t h = jobs.parallel_reduce("hash", n, 4096, std::uint64_t{0},
    [&](std::size_t begin, std::size_t end) { std::uint64_t p = 0; /* ... */ return p; },
    [](std::uint64_t acc, std::uint64_t p) { return veach::hash(acc, p); });  // chunk order

veach::TaskGroup group(jobs);
group.run("cook collider", [&] { /* ... */ });
group.wait();
```

```sh
veach-lab --jobs 1 ...      # single-threaded: every job on the main thread, in chunk order
```

```json
{ "jobs": 8 }
```

## Acceptance criteria

- **jobs.AC1**: Hashing 10⁶ items with `veach::hash`, reduced as a hash chain and as a floating-point sum, gives the
  same bits on 1, 2, 3, the default and twice the default threads, equal to a plain loop over the same chunks;
  `parallel_for` into indexed slots matches too.
- **jobs.AC2**: Chunk ranges depend only on count and grain (`[k·grain, min(count, (k+1)·grain))`), for every thread
  count; empty ranges call nothing.
- **jobs.AC3**: Nested `parallel_for` calls, task-group jobs that queue more jobs, and `wait_idle()` all complete
  every job before returning.
- **jobs.AC4**: `jobs` comes from the project file and `--jobs` (CLI wins); out-of-range values explain the limit;
  with one thread every job runs on the main thread; with workers, jobs run only on threads the job system owns.
- **jobs.AC5**: The engine provides the job system from the `jobs` init step (second), finishes running jobs
  before `shutdown ecs`, and shuts the job system down second to last.
- **jobs.AC6**: Canonical floating point (dmath, subnormals) gives the same bits on workers as on the main thread.

`veach-bench-jobs` (release) prints ms and speedup per thread count for the canonical workload at 10⁷ items and
exits 1 if any result differs.

## Out of scope

- Flecs `multi_threaded()` systems on the job system's workers (VCH-107).
- Jolt's `JobSystemWithBarrier` adapter (VCH-087).
- Background priorities and polling for streaming (VCH-031 and terrain streaming add them when they need them).

## Open questions

None.

## Links

- ADRs: [0073](../../adr/0073-job-system-enkits.md), [0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md),
  [0018](../../adr/0018-engine-lifecycle-and-phases.md)
- Backlog: VCH-015, VCH-107, VCH-087, VCH-031
- Guides: [extending](../../guides/extending.md#j-parallel-work-jobs)
