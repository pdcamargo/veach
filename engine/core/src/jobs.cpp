#include <veach/core/jobs.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/profile.hpp>

#include <TaskScheduler.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <thread>

// enkiTS creates the worker threads (docs/adr/0073-job-system-enkits.md); this file is the only place in the
// engine that starts long-lived threads (tools/scripts/check_repo.py enforces it).

namespace veach {

namespace detail {

/// One TaskGroup job. An enkiTS task set of size 1, heap-allocated so its address stays put while it runs.
struct GroupJob final : enki::ITaskSet {
    GroupJob(std::string_view label_, std::function<void()> job_) : label(label_), job(std::move(job_)) {}
    void ExecuteRange(enki::TaskSetPartition /*range*/, std::uint32_t /*thread*/) override {
        VEACH_ZONE_TEXT("job", label.data(), label.size());
        job();
    }
    std::string_view label;
    std::function<void()> job;
};

}  // namespace detail

namespace {

/// A parallel_for: one enkiTS set over chunk indices. enkiTS splits the set into ranges that depend on its thread
/// count; each range only loops over whole chunks, so the item ranges handed to the body never do.
struct ChunkTask final : enki::ITaskSet {
    void ExecuteRange(enki::TaskSetPartition range, std::uint32_t /*thread*/) override {
        VEACH_ZONE_TEXT("jobs", label.data(), label.size());
        for (std::uint32_t chunk = range.start; chunk < range.end; ++chunk) {
            const std::size_t begin = std::size_t{chunk} * grain;
            fn(context, chunk, begin, std::min(count, begin + grain));
        }
    }
    std::string_view label;
    std::size_t count = 0;
    std::size_t grain = 1;
    detail::ChunkFn fn = nullptr;
    void* context = nullptr;
};

void on_worker_start(std::uint32_t thread) {
#if VEACH_PROFILER_ENABLED
    const std::string name = "veach worker " + std::to_string(thread);
    VEACH_THREAD_NAME(name.c_str());
#else
    static_cast<void>(thread);
#endif
}

}  // namespace

std::uint32_t default_job_threads() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::clamp<std::uint32_t>(hardware > 1 ? hardware - 1 : 1, 1, max_job_threads);
}

struct JobSystem::Impl {
    enki::TaskScheduler scheduler;
    std::uint32_t threads = 1;
    std::atomic<std::uint32_t> groups{0};  ///< Live TaskGroups; none may outlive the job system
};

JobSystem::JobSystem(std::uint32_t threads) : impl_(std::make_unique<Impl>()) {
    if (threads == 0) {
        threads = default_job_threads();
    }
    VEACH_ASSERT(threads <= max_job_threads, "{} job threads requested; the limit is {}", threads, max_job_threads);
    impl_->threads = threads;
    enki::TaskSchedulerConfig config;
    config.numTaskThreadsToCreate = threads - 1;
    config.profilerCallbacks.threadStart = &on_worker_start;
    impl_->scheduler.Initialize(config);
}

JobSystem::~JobSystem() {
    VEACH_ASSERT(impl_->groups.load() == 0, "a TaskGroup outlives the JobSystem it runs on");
    impl_->scheduler.WaitforAllAndShutdown();
}

std::uint32_t JobSystem::thread_count() const {
    return impl_->threads;
}

bool JobSystem::on_job_thread() const {
    return impl_->scheduler.GetThreadNum() != enki::NO_THREAD_NUM;
}

std::uint32_t JobSystem::thread_index() const {
    VEACH_ASSERT(on_job_thread(), "thread_index() called from a thread the job system does not own");
    return impl_->scheduler.GetThreadNum();
}

std::size_t JobSystem::chunk_count(std::size_t count, std::size_t grain) {
    VEACH_ASSERT(grain > 0, "a parallel_for needs a grain of at least 1 item");
    return count / grain + (count % grain != 0 ? 1 : 0);
}

void JobSystem::run_chunks(std::string_view label, std::size_t count, std::size_t grain, detail::ChunkFn fn,
                           void* context) {
    const std::size_t chunks = chunk_count(count, grain);
    if (chunks == 0) {
        return;
    }
    VEACH_ASSERT(on_job_thread(), "'{}': jobs are submitted from the main thread or from jobs", label);
    VEACH_ASSERT(chunks <= std::numeric_limits<std::uint32_t>::max(), "'{}': {} chunks; use a larger grain", label,
                 chunks);
    if (impl_->threads == 1 || chunks == 1) {
        VEACH_ZONE_TEXT("jobs", label.data(), label.size());
        for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
            const std::size_t begin = chunk * grain;
            fn(context, chunk, begin, std::min(count, begin + grain));
        }
        return;
    }
    ChunkTask task;
    task.m_SetSize = static_cast<std::uint32_t>(chunks);
    task.m_MinRange = 1;
    task.label = label;
    task.count = count;
    task.grain = grain;
    task.fn = fn;
    task.context = context;
    impl_->scheduler.AddTaskSetToPipe(&task);
    impl_->scheduler.WaitforTask(&task);
}

void JobSystem::wait_idle() {
    VEACH_ASSERT(thread_index() == 0, "wait_idle() is for the thread that owns the job system");
    VEACH_ZONE("JobSystem::wait_idle");
    impl_->scheduler.WaitforAll();
}

TaskGroup::TaskGroup(JobSystem& jobs) : jobs_(jobs) {
    ++jobs_.impl_->groups;
}

TaskGroup::~TaskGroup() {
    wait();
    --jobs_.impl_->groups;
}

void TaskGroup::run(std::string_view label, std::function<void()> job) {
    VEACH_ASSERT(jobs_.on_job_thread(), "'{}': jobs are submitted from the main thread or from jobs", label);
    auto queued = std::make_unique<detail::GroupJob>(label, std::move(job));
    detail::GroupJob* task = queued.get();
    {
        const std::scoped_lock lock(mutex_);
        jobs_queued_.push_back(std::move(queued));
    }
    jobs_.impl_->scheduler.AddTaskSetToPipe(task);
}

void TaskGroup::wait() {
    // Jobs may queue more jobs into this group while it is waited on: keep going until the list stops growing.
    for (std::size_t i = 0;; ++i) {
        detail::GroupJob* task = nullptr;
        {
            const std::scoped_lock lock(mutex_);
            if (i >= jobs_queued_.size()) {
                jobs_queued_.clear();
                return;
            }
            task = jobs_queued_[i].get();
        }
        jobs_.impl_->scheduler.WaitforTask(task);
    }
}

}  // namespace veach
