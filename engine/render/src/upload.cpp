#include <veach/render/upload.hpp>

#include <veach/core/log.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace veach::render {

namespace {

constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

UploadRing::UploadRing(rhi::Device& device, std::uint64_t bytes_per_frame)
    : device_(device),
      bytes_per_frame_(align_up(std::max<std::uint64_t>(bytes_per_frame, rhi::texture_copy_offset_alignment),
                                rhi::texture_copy_offset_alignment)),
      slots_(std::max(1u, device.frames_in_flight())) {}

UploadRing::~UploadRing() {
    for (Slot& slot : slots_) {
        for (const rhi::BufferHandle b : slot.overflow) {
            device_.destroy(b);
        }
    }
    if (buffer_.is_valid()) {
        device_.destroy(buffer_);
    }
}

UploadRing::Slot& UploadRing::current() {
    const rhi::FrameInfo frame = device_.current_frame();
    Slot& slot = slots_.at(frame.slot % slots_.size());
    if (slot.frame != frame.index) {
        // begin_frame() waited for the frame that used this slot last: its memory is free again.
        slot.frame = frame.index;
        slot.head = 0;
        slot.overflow_bytes = 0;
        for (const rhi::BufferHandle b : slot.overflow) {
            device_.destroy(b);
        }
        slot.overflow.clear();
    }
    return slot;
}

Result<UploadRing::Allocation> UploadRing::allocate(std::uint64_t size, std::uint64_t alignment) {
    alignment = std::max<std::uint64_t>(alignment, 1);
    Slot& slot = current();
    const auto index = static_cast<std::uint64_t>(&slot - slots_.data());
    if (!buffer_.is_valid()) {
        auto buffer = device_.create_buffer({.size = bytes_per_frame_ * slots_.size(),
                                             .usage = rhi::BufferUsage::copy_src,
                                             .memory = rhi::MemoryType::upload,
                                             .debug_name = "upload ring"});
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        buffer_ = *buffer;
        mapped_ = static_cast<std::byte*>(device_.map(buffer_));
    }
    const std::uint64_t base = index * bytes_per_frame_;
    const std::uint64_t offset = align_up(base + slot.head, alignment);
    if (offset + size <= base + bytes_per_frame_ && mapped_ != nullptr) {
        slot.head = offset + size - base;
        return Allocation{buffer_, offset, mapped_ + offset};
    }
    // Beyond this frame's share: a buffer of its own, kept until the slot comes round.
    auto own = device_.create_buffer({.size = std::max<std::uint64_t>(size, 16),
                                      .usage = rhi::BufferUsage::copy_src,
                                      .memory = rhi::MemoryType::upload,
                                      .debug_name = "upload ring (overflow)"});
    if (!own) {
        return std::unexpected(own.error());
    }
    slot.overflow.push_back(*own);
    slot.overflow_bytes += size;
    log::debug("render", "upload ring: {} bytes beyond the frame's {} went to a buffer of their own", size,
               bytes_per_frame_);
    return Allocation{*own, 0, static_cast<std::byte*>(device_.map(*own))};
}

std::uint64_t UploadRing::used() const {
    const rhi::FrameInfo frame = device_.current_frame();
    const Slot& slot = slots_.at(frame.slot % slots_.size());
    return slot.frame == frame.index ? slot.head : 0;
}

std::uint64_t UploadRing::overflow() const {
    const rhi::FrameInfo frame = device_.current_frame();
    const Slot& slot = slots_.at(frame.slot % slots_.size());
    return slot.frame == frame.index ? slot.overflow_bytes : 0;
}

std::uint64_t region_bytes(rhi::Format format, const rhi::TextureRegion& region) {
    return rhi::row_bytes(format, region.width) * rhi::row_count(format, region.height) * std::max(1u, region.depth) *
           region.layer_count;
}

Result<> upload_texture(RenderGraph& graph, UploadRing& ring, GraphResource texture,
                        const rhi::TextureRegion& requested, std::span<const std::byte> data, std::string pass_name) {
    const rhi::TextureDesc* desc = graph.texture_desc(texture);
    if (desc == nullptr) {
        return make_error(ErrorCode::render_graph_invalid,
                          std::format("upload_texture '{}': the resource is not a texture of this graph", pass_name),
                          "Pass a GraphResource from graph.import(texture, ...) or graph.create_texture().");
    }
    auto region = rhi::resolve_region(*desc, requested);
    if (!region) {
        return std::unexpected(region.error());
    }
    const std::uint64_t expected = region_bytes(desc->format, *region);
    if (data.size() != expected) {
        return make_error(
            ErrorCode::rhi_invalid_texture,
            std::format("upload_texture '{}' into '{}': {} bytes of data for a region of {}", pass_name,
                        desc->debug_name, data.size(), expected),
            "Upload data is tightly packed: layers, then depth slices, then rows (render::region_bytes).");
    }
    const std::uint64_t row = rhi::row_bytes(desc->format, region->width);
    const std::uint32_t rows = rhi::row_count(desc->format, region->height);
    const std::uint32_t pitch = rhi::texture_copy_pitch(desc->format, region->width);
    const std::uint64_t stride = rhi::texture_copy_layer_stride(desc->format, *region, pitch);
    auto staged = ring.allocate(stride * region->layer_count);
    if (!staged) {
        return std::unexpected(staged.error());
    }
    if (staged->data == nullptr) {
        return make_error(ErrorCode::rhi_gpu_error, "upload_texture: the upload ring's memory is not mapped");
    }
    const std::byte* from = data.data();
    for (std::uint32_t layer = 0; layer < region->layer_count; ++layer) {
        std::byte* to = staged->data + stride * layer;
        for (std::uint32_t line = 0; line < rows * region->depth; ++line) {  // rows of every depth slice
            std::memcpy(to + std::uint64_t{line} * pitch, from, row);
            from += row;
        }
    }
    const GraphResource source = graph.import(staged->buffer, pass_name + " (staging)", rhi::ResourceState::copy_src);
    graph.add_transfer_pass(
        std::move(pass_name),
        [&](PassBuilder& b) {
            b.read(source, rhi::ResourceState::copy_src);
            b.write(texture, rhi::ResourceState::copy_dst);
        },
        [source, texture, offset = staged->offset, pitch, copy = *region](PassContext& ctx) {
            ctx.cmd.copy_buffer_to_texture(ctx.buffer(source), offset, pitch, ctx.texture(texture), copy);
        });
    return {};
}

}  // namespace veach::render
