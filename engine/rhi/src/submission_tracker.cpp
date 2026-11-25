#include <veach/rhi/submission_tracker.hpp>

#include <veach/core/assert.hpp>

#include <algorithm>

namespace veach::rhi {

SubmissionTracker::SubmissionTracker(std::uint32_t frames_in_flight)
    : frame_fences_(std::max(1u, frames_in_flight), 0) {}

SubmissionId SubmissionTracker::next_submission() {
    return {++submitted_};
}

void SubmissionTracker::mark_completed(SubmissionId id) {
    std::uint64_t current = completed_.load(std::memory_order_relaxed);
    while (id.value > current &&
           !completed_.compare_exchange_weak(current, id.value, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

FrameInfo SubmissionTracker::begin_frame(SubmissionId& wait_for) {
    ++frame_.index;
    frame_.slot = static_cast<std::uint32_t>(frame_.index % frame_fences_.size());
    const std::uint64_t fence = frame_fences_[frame_.slot];
    wait_for = SubmissionId{fence > completed().value ? fence : 0};
    return frame_;
}

void SubmissionTracker::end_frame() {
    frame_fences_[frame_.slot] = submitted_;
}

void SubmissionTracker::retire(std::function<void()> release) {
    if (submitted_ <= completed().value) {
        release();  // nothing in flight can be using it
        return;
    }
    releases_.push_back({submitted_, std::move(release)});
}

void SubmissionTracker::collect() {
    const std::uint64_t done = completed().value;
    while (!releases_.empty() && releases_.front().after <= done) {
        auto fn = std::move(releases_.front().fn);
        releases_.pop_front();
        fn();
    }
}

}  // namespace veach::rhi
