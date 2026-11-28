#pragma once

// veach::JobSystem: the engine's worker threads (docs/adr/0073-job-system-enkits.md, docs/specs/job-system/spec.md).
// The engine creates one in the `jobs` init step and provides it as a service; nothing else in the process creates
// threads. Find it with services(world).get<JobSystem>().
//
//   jobs.parallel_for("mesh chunks", chunks.size(), 1, [&](std::size_t begin, std::size_t end) {
//       for (std::size_t i = begin; i < end; ++i) meshes[i] = mesh(chunks[i]);   // indexed slots
//   });
//   const std::uint64_t h = jobs.parallel_reduce("hash", n, 4096, std::uint64_t{0},
//       [&](std::size_t begin, std::size_t end) { ... return partial; },
//       [](std::uint64_t acc, std::uint64_t partial) { return veach::hash(acc, partial); });
//
// Rules:
// - Results never depend on the thread count. Work is cut into fixed chunks of `grain` items ([0, grain),
//   [grain, 2 grain), ...) whatever the number of threads, and parallel_reduce combines the chunk results in chunk
//   order on the calling thread. Write outputs to indexed slots; never append to shared containers from jobs.
// - Jobs never touch the World. They read inputs prepared before the call and write outputs read after it; the
//   main thread applies results to the world.
// - Only the thread that created the job system and the job threads may submit and wait. A job may run its own
//   parallel_for or TaskGroup (the waiting thread runs other jobs meanwhile).
// - Jobs must not throw: an exception escaping a job terminates the process.
// - With one thread (--jobs 1) there are no workers: parallel_for runs the chunks in order on the caller, and
//   TaskGroup jobs run when the group is waited on.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace veach {

/// The most threads a job system may have.
inline constexpr std::uint32_t max_job_threads = 256;

/// Threads used when the project and the command line leave `jobs` at 0: the hardware threads minus one (left
/// for the OS and the GPU driver), at least 1.
[[nodiscard]] std::uint32_t default_job_threads();

namespace detail {
/// Runs the items [begin, end) of chunk `chunk`.
using ChunkFn = void (*)(void* context, std::size_t chunk, std::size_t begin, std::size_t end) noexcept;
struct GroupJob;
}  // namespace detail

class JobSystem {
public:
    /// `threads` counts the calling thread, which runs jobs while it waits: 1 means no workers. 0 picks
    /// default_job_threads(). Must be at most max_job_threads.
    explicit JobSystem(std::uint32_t threads = 0);
    /// Waits for every job, then joins the workers.
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    /// Threads that run jobs, the owner included.
    [[nodiscard]] std::uint32_t thread_count() const;
    [[nodiscard]] std::uint32_t worker_count() const { return thread_count() - 1; }
    /// True on the owner thread and on the workers: the threads that may submit and wait.
    [[nodiscard]] bool on_job_thread() const;
    /// 0 on the owner, 1..worker_count() on the workers. For per-thread scratch memory only, never for results.
    [[nodiscard]] std::uint32_t thread_index() const;

    /// Calls body(begin, end) once per chunk of `grain` items of [0, count) and returns when all are done. Chunks
    /// run concurrently in any order; the cut does not depend on the thread count.
    template <typename Body>
        requires std::is_invocable_v<Body&, std::size_t, std::size_t>
    void parallel_for(std::string_view label, std::size_t count, std::size_t grain, Body&& body) {
        run_chunks(label, count, grain, &call_for<std::remove_reference_t<Body>>, std::addressof(body));
    }

    /// Map-reduce in index order: partial_k = map(begin_k, end_k) per chunk, run in parallel, then
    /// combine(...combine(combine(init, partial_0), partial_1)..., partial_last) on the calling thread. The result
    /// is the same for any thread count, even when combine is not associative (floating-point sums, hash chains).
    template <typename T, typename Map, typename Combine>
        requires std::is_invocable_r_v<T, Map&, std::size_t, std::size_t> && std::is_invocable_r_v<T, Combine&, T, T>
    [[nodiscard]] T parallel_reduce(std::string_view label, std::size_t count, std::size_t grain, T init, Map&& map,
                                    Combine&& combine) {
        static_assert(!std::is_same_v<T, bool>, "reduce into an integer: std::vector<bool> has no slots");
        if (count == 0) {
            return init;
        }
        std::vector<T> partials(chunk_count(count, grain), init);
        ReduceContext<T, std::remove_reference_t<Map>> context{&map, partials.data()};
        run_chunks(label, count, grain, &call_reduce<T, std::remove_reference_t<Map>>, &context);
        T result = std::move(init);
        for (T& partial : partials) {
            result = combine(std::move(result), std::move(partial));
        }
        return result;
    }

    /// Runs and waits for every queued job (owner thread only). The engine calls it before tearing down the world.
    void wait_idle();

    /// Chunks of `grain` items needed for `count` items.
    [[nodiscard]] static std::size_t chunk_count(std::size_t count, std::size_t grain);

private:
    friend class TaskGroup;
    struct Impl;

    template <typename Body>
    static void call_for(void* context, std::size_t /*chunk*/, std::size_t begin, std::size_t end) noexcept {
        (*static_cast<Body*>(context))(begin, end);
    }
    template <typename T, typename Map>
    struct ReduceContext {
        Map* map;
        T* partials;
    };
    template <typename T, typename Map>
    static void call_reduce(void* context, std::size_t chunk, std::size_t begin, std::size_t end) noexcept {
        auto* c = static_cast<ReduceContext<T, Map>*>(context);
        c->partials[chunk] = (*c->map)(begin, end);
    }

    void run_chunks(std::string_view label, std::size_t count, std::size_t grain, detail::ChunkFn fn, void* context);

    std::unique_ptr<Impl> impl_;
};

/// A set of independent jobs to wait for together. run() may be called from any job thread, including from a job
/// of this group (Jolt queues jobs from inside jobs); wait() from the thread that created the group. The
/// destructor waits.
class TaskGroup {
public:
    explicit TaskGroup(JobSystem& jobs);
    ~TaskGroup();
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;

    /// Queues `job`. `label` names its Tracy zone and must outlive the job (a string literal).
    void run(std::string_view label, std::function<void()> job);
    /// Returns once every job queued so far, and every job those jobs queued, has finished.
    void wait();

private:
    JobSystem& jobs_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<detail::GroupJob>> jobs_queued_;
};

}  // namespace veach
