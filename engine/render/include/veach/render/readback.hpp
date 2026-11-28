#pragma once

// Non-blocking GPU-to-CPU readback (VCH-073, ADR 0089). A pass's buffer is copied into a staging buffer at the end of
// the frame's graph; the bytes reach a callback exactly `latency()` (= frames in flight) frames later, when
// Device::begin_frame has already waited for that frame, so delivery never stalls the CPU and arrives on the same
// frame on every backend and GPU. Callbacks run on the render thread inside Renderer::render(); they copy the bytes
// into the module's own CPU state, never into the World and never into anything canonical (ADR 0043).
//
//   void record(RenderGraph& graph, const FrameContext& frame) override {
//       frame.readbacks->read(graph, counts, sizeof(std::uint32_t), [this](Result<ReadbackData> data) {
//           if (data) { std::memcpy(&visible_, data->bytes.data(), sizeof(visible_)); }
//       });
//   }

#include <veach/core/error.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/rhi/rhi.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace veach::render {

/// The bytes a readback copied, and the frame (Device::current_frame().index) whose graph copied them.
struct ReadbackData {
    std::uint64_t frame = 0;
    std::vector<std::byte> bytes;  ///< zeros on the null RHI, which runs nothing
};

/// Called once per request: with the data, or with render.readback_dropped if its frame was never submitted.
using ReadbackCallback = std::function<void(Result<ReadbackData>)>;

class ReadbackQueue {
public:
    explicit ReadbackQueue(rhi::Device& device);
    /// Destroys the staging buffers (deferred until the GPU is done). Undelivered requests are dropped silently:
    /// call flush() first to deliver them.
    ~ReadbackQueue();
    ReadbackQueue(const ReadbackQueue&) = delete;
    ReadbackQueue& operator=(const ReadbackQueue&) = delete;
    ReadbackQueue(ReadbackQueue&&) = delete;
    ReadbackQueue& operator=(ReadbackQueue&&) = delete;

    /// Copies the first `size` bytes of `buffer` (BufferUsage::copy_src) at the end of this frame's graph, in a
    /// transfer pass named `name`. Between Device::begin_frame() and the frame's submit.
    void read(RenderGraph& graph, GraphResource buffer, std::uint64_t size, ReadbackCallback done,
              std::string name = "readback");
    /// The frame's graph was submitted as `submission`: its requests are now in flight.
    void submitted(rhi::SubmissionId submission);
    /// The frame's graph was not submitted: its requests fail with render.readback_dropped.
    void abandon();
    /// Delivers the requests made `latency()` or more frames ago. Call right after Device::begin_frame(), which has
    /// waited for their frame, so this never waits (if one is somehow not complete, it waits and counts a stall).
    void deliver();
    /// Waits for every request in flight and delivers it now (shutdown, tests, a blocking capture).
    void flush();

    /// Frames between a request and its delivery: the device's frames in flight.
    [[nodiscard]] std::uint32_t latency() const { return device_.frames_in_flight(); }
    /// Requests recorded or in flight, not yet delivered.
    [[nodiscard]] std::size_t pending() const { return requests_.size(); }
    /// Times deliver() found a due request still running and had to wait; 0 unless frames were skipped.
    [[nodiscard]] std::uint64_t stalls() const { return stalls_; }
    /// Staging buffers made so far (reused once delivered).
    [[nodiscard]] std::size_t staging_buffers() const { return staging_.size(); }

private:
    struct Staging {
        rhi::BufferHandle buffer;
        std::uint64_t size = 0;
        bool busy = false;
    };
    struct Request {
        std::uint32_t staging = 0;
        std::uint64_t size = 0;
        std::uint64_t frame = 0;
        rhi::SubmissionId submission;  ///< 0 while its frame is being recorded
        ReadbackCallback done;
    };

    Result<std::uint32_t> acquire(std::uint64_t size);
    void complete(Request& request);

    rhi::Device& device_;
    std::vector<Staging> staging_;
    std::vector<Request> requests_;  ///< in request order
    std::uint64_t stalls_ = 0;
};

}  // namespace veach::render
