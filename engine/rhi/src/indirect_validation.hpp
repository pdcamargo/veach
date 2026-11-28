#pragma once

#include <veach/core/assert.hpp>
#include <veach/rhi/types.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

namespace veach::rhi {

/// Every backend checks indirect arguments the same way before recording (VCH-071), so a call that works on one
/// works on all: `count` records of `record_size` bytes, `stride` apart from `offset`, inside an indirect buffer.
inline void check_indirect_args(const BufferDesc* args, std::uint64_t offset, std::uint32_t count, std::uint32_t stride,
                                std::uint32_t record_size, [[maybe_unused]] std::string_view call) {
    VEACH_ASSERT(args != nullptr, "{}: invalid argument buffer", call);
    VEACH_ASSERT(has(args->usage, BufferUsage::indirect), "{}: buffer '{}' was not created with BufferUsage::indirect",
                 call, args->debug_name);
    VEACH_ASSERT(offset % 4 == 0 && stride % 4 == 0, "{}: offset {} and stride {} must be multiples of 4", call, offset,
                 stride);
    VEACH_ASSERT(stride >= record_size, "{}: stride {} is smaller than a {}-byte record", call, stride, record_size);
    VEACH_ASSERT(count == 0 || offset + std::uint64_t{count - 1} * stride + record_size <= args->size,
                 "{}: {} records from offset {} do not fit in '{}' ({} bytes)", call, count, offset, args->debug_name,
                 args->size);
}

/// The index buffer of draw_indexed_indirect: uint32 indices in a BufferUsage::index buffer.
inline void check_index_buffer(const BufferDesc* indices, [[maybe_unused]] std::string_view call) {
    VEACH_ASSERT(indices != nullptr, "{}: invalid index buffer", call);
    VEACH_ASSERT(has(indices->usage, BufferUsage::index), "{}: buffer '{}' was not created with BufferUsage::index",
                 call, indices->debug_name);
}

/// copy_buffer_regions: every region 4-byte aligned and inside both buffers.
inline void check_buffer_copies([[maybe_unused]] const BufferDesc* src, [[maybe_unused]] const BufferDesc* dst,
                                std::span<const BufferCopy> regions) {
    VEACH_ASSERT(src != nullptr && dst != nullptr, "copy_buffer_regions: invalid buffers");
    for ([[maybe_unused]] const BufferCopy& r : regions) {
        VEACH_ASSERT(r.src_offset % 4 == 0 && r.dst_offset % 4 == 0 && r.size % 4 == 0,
                     "copy_buffer_regions: offsets {} and {} and size {} must be multiples of 4", r.src_offset,
                     r.dst_offset, r.size);
        VEACH_ASSERT(r.src_offset + r.size <= src->size && r.dst_offset + r.size <= dst->size,
                     "copy_buffer_regions: {} bytes from {} to {} do not fit '{}' ({} bytes) and '{}' ({} bytes)",
                     r.size, r.src_offset, r.dst_offset, src->debug_name, src->size, dst->debug_name, dst->size);
    }
}

/// A barrier's mips lie inside its texture (null: a texture the backend does not describe, a swapchain image).
inline void check_barrier_mips([[maybe_unused]] const TextureDesc* texture, [[maybe_unused]] const Barrier& b) {
    VEACH_ASSERT(
        texture == nullptr || b.mip_count == 0 || b.first_mip + b.mip_count <= std::max(1u, texture->mip_levels),
        "barrier on mips {}..{} of '{}', which has {}", b.first_mip, b.first_mip + b.mip_count, texture->debug_name,
        texture->mip_levels);
}

}  // namespace veach::rhi
