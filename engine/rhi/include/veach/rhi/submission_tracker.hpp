#pragma once

// Frames in flight and resource retirement (ADR 0030). Backend-independent:
// Metal and DX12 both build on it, so the rules are the same everywhere.
//
// - Every submission gets a monotonically increasing SubmissionId (a DX12 fence
//   value; on Metal the command buffer's completion handler reports it).
// - The GPU completes submissions in order on our single queue, so "completed"
//   is one watermark.
// - Frame N may start recording once frame N - frames_in_flight has completed.
// - destroy() never waits: the release runs once every submission made before it
//   has completed.

#include <veach/rhi/types.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace veach::rhi {

struct FrameInfo {
    std::uint64_t index = 0;  ///< 1, 2, 3, ... (frames begun so far)
    std::uint32_t slot = 0;   ///< index % frames_in_flight: which per-frame resources to use
};

class SubmissionTracker {
public:
    explicit SubmissionTracker(std::uint32_t frames_in_flight);

    [[nodiscard]] std::uint32_t frames_in_flight() const { return static_cast<std::uint32_t>(frame_fences_.size()); }

    /// Allocates the id for a submission about to be made.
    [[nodiscard]] SubmissionId next_submission();
    [[nodiscard]] SubmissionId last_submitted() const { return {submitted_}; }

    /// Reports that `id` (and so every earlier submission) finished. Thread-safe.
    void mark_completed(SubmissionId id);
    [[nodiscard]] SubmissionId completed() const { return {completed_.load(std::memory_order_acquire)}; }
    [[nodiscard]] bool is_complete(SubmissionId id) const { return id.value <= completed().value; }

    /// Starts frame N. Returns the submission the caller must wait for before
    /// reusing slot N % frames_in_flight: frame N - frames_in_flight's last one,
    /// or nothing (value 0) if that is already complete.
    [[nodiscard]] FrameInfo begin_frame(SubmissionId& wait_for);
    /// Records the last submission made so far as the current frame's fence.
    void end_frame();
    [[nodiscard]] FrameInfo current_frame() const { return frame_; }

    /// Defers `release` until every submission made so far has completed.
    void retire(std::function<void()> release);
    /// Runs the releases that are now safe. Call on the owning thread.
    void collect();
    [[nodiscard]] std::size_t pending_releases() const { return releases_.size(); }

private:
    struct Release {
        std::uint64_t after = 0;
        std::function<void()> fn;
    };

    std::vector<std::uint64_t> frame_fences_;  ///< per slot: last submission of the frame that used it
    std::uint64_t submitted_ = 0;
    std::atomic<std::uint64_t> completed_{0};
    FrameInfo frame_;
    std::deque<Release> releases_;  ///< ordered by `after`
};

}  // namespace veach::rhi
