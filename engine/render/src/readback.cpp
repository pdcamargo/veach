#include <veach/render/readback.hpp>

#include <veach/core/log.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <utility>

namespace veach::render {

namespace {

/// Moves the requests `pred` selects out of `from`, keeping the order of both.
template <class Request, class Pred>
std::vector<Request> take(std::vector<Request>& from, Pred pred) {
    std::vector<Request> taken;
    std::vector<Request> kept;
    for (Request& r : from) {
        (pred(r) ? taken : kept).push_back(std::move(r));
    }
    from = std::move(kept);
    return taken;
}

}  // namespace

ReadbackQueue::ReadbackQueue(rhi::Device& device) : device_(device) {}

ReadbackQueue::~ReadbackQueue() {
    for (const Staging& s : staging_) {
        device_.destroy(s.buffer);
    }
}

Result<std::uint32_t> ReadbackQueue::acquire(std::uint64_t size) {
    for (std::uint32_t i = 0; i < staging_.size(); ++i) {
        if (!staging_[i].busy && staging_[i].size >= size) {
            staging_[i].busy = true;
            return i;
        }
    }
    // Powers of two from 256 bytes, so requests of similar sizes share buffers.
    const std::uint64_t bytes = std::max<std::uint64_t>(256, std::bit_ceil(size));
    auto buffer = device_.create_buffer({.size = bytes,
                                         .usage = rhi::BufferUsage::copy_dst,
                                         .memory = rhi::MemoryType::readback,
                                         .debug_name = std::format("readback staging {}", staging_.size())});
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    staging_.push_back({*buffer, bytes, true});
    return static_cast<std::uint32_t>(staging_.size() - 1);
}

void ReadbackQueue::read(RenderGraph& graph, GraphResource buffer, std::uint64_t size, ReadbackCallback done,
                         std::string name) {
    auto staging = acquire(size);
    if (!staging) {
        done(std::unexpected(staging.error()));
        return;
    }
    const GraphResource target =
        graph.import(staging_[*staging].buffer, name + " (staging)", rhi::ResourceState::undefined);
    graph.add_transfer_pass(
        std::move(name),
        [&](PassBuilder& b) {
            b.read(buffer, rhi::ResourceState::copy_src);
            b.write(target, rhi::ResourceState::copy_dst);
        },
        [buffer, target, size](PassContext& ctx) {
            ctx.cmd.copy_buffer(ctx.buffer(buffer), ctx.buffer(target), size);
        });
    requests_.push_back({*staging, size, device_.current_frame().index, {}, std::move(done)});
}

void ReadbackQueue::submitted(rhi::SubmissionId submission) {
    for (Request& r : requests_) {
        if (r.submission.value == 0) {
            r.submission = submission;
        }
    }
}

void ReadbackQueue::abandon() {
    std::vector<Request> dropped = take(requests_, [](const Request& r) { return r.submission.value == 0; });
    for (Request& r : dropped) {
        staging_.at(r.staging).busy = false;
        r.done(make_error(
            ErrorCode::render_readback_dropped,
            std::format("a {}-byte readback of frame {} was dropped: the frame was not submitted", r.size, r.frame),
            "The frame's render graph failed to compile; the error before this one says why."));
    }
}

void ReadbackQueue::complete(Request& request) {
    Staging& staging = staging_.at(request.staging);
    ReadbackData data{request.frame, std::vector<std::byte>(request.size)};
    if (const auto* bytes = static_cast<const std::byte*>(device_.map(staging.buffer))) {
        std::memcpy(data.bytes.data(), bytes, request.size);
    }
    staging.busy = false;
    request.done(std::move(data));
}

void ReadbackQueue::deliver() {
    const std::uint64_t now = device_.current_frame().index;
    std::vector<Request> due =
        take(requests_, [&](const Request& r) { return r.submission.value != 0 && r.frame + latency() <= now; });
    for (Request& r : due) {
        if (!device_.is_complete(r.submission)) {
            // Only possible if frames were begun out of the renderer's order; still correct, but a stall.
            log::warn("render", "readback of frame {} was not complete at frame {}; waiting for it", r.frame, now);
            ++stalls_;
            device_.wait(r.submission);
        }
        complete(r);
    }
}

void ReadbackQueue::flush() {
    std::vector<Request> in_flight = take(requests_, [](const Request& r) { return r.submission.value != 0; });
    for (Request& r : in_flight) {
        device_.wait(r.submission);
        complete(r);
    }
}

}  // namespace veach::render
