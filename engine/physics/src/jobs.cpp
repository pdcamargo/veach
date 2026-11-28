// Jolt's JobSystemWithBarrier over veach::JobSystem (see jolt.hpp).

#include "jolt.hpp"

#include <veach/core/jobs.hpp>

#include <algorithm>
#include <thread>

namespace veach::physics::detail {

JoltJobs::JoltJobs(veach::JobSystem* jobs, std::uint32_t max_concurrency)
    : jobs_(jobs), max_concurrency_(max_concurrency) {
    JobSystemWithBarrier::Init(JPH::cMaxPhysicsBarriers);
    free_.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
}

JoltJobs::~JoltJobs() {
    end();
}

void JoltJobs::begin(bool parallel) {
    if (parallel && jobs_ != nullptr && jobs_->worker_count() > 0) {
        group_ = std::make_unique<veach::TaskGroup>(*jobs_);
    }
}

void JoltJobs::end() {
    if (group_ != nullptr) {
        group_->wait();
        group_.reset();
    }
}

int JoltJobs::GetMaxConcurrency() const {
    return jobs_ != nullptr ? static_cast<int>(std::max(1u, std::min(jobs_->thread_count(), max_concurrency_))) : 1;
}

JPH::JobHandle JoltJobs::CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                                   JPH::uint32 dependencies) {
    JPH::uint32 index = free_.ConstructObject(name, color, this, function, dependencies);
    while (index == decltype(free_)::cInvalidObjectIndex) {
        // Every job slot is taken (cMaxPhysicsJobs): let the workers finish some. Jolt's own pools do the same.
        std::this_thread::yield();
        index = free_.ConstructObject(name, color, this, function, dependencies);
    }
    Job* job = &free_.Get(index);
    JobHandle handle(job);
    if (dependencies == 0) {
        QueueJob(job);
    }
    return handle;
}

void JoltJobs::CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& function, JobHandle* out,
                          JPH::uint32 count, JPH::uint32 dependencies) {
    for (JPH::uint32 i = 0; i < count; ++i) {
        out[i] = CreateJob(name, color, function, dependencies);
    }
}

void JoltJobs::QueueJob(Job* job) {
    if (group_ == nullptr) {
        // No workers (or outside an update): run it here. The barrier would run it anyway.
        job->Execute();
        return;
    }
    job->AddRef();
    group_->run("jolt job", [job] {
        job->Execute();  // a no-op if a barrier already ran it
        job->Release();
    });
}

void JoltJobs::QueueJobs(Job** jobs, JPH::uint count) {
    for (JPH::uint i = 0; i < count; ++i) {
        QueueJob(jobs[i]);
    }
}

void JoltJobs::FreeJob(Job* job) {
    free_.DestructObject(job);
}

}  // namespace veach::physics::detail
