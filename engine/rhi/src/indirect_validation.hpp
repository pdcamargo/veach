#pragma once

#include <veach/core/assert.hpp>
#include <veach/rhi/types.hpp>

#include <cstdint>
#include <string_view>

namespace veach::rhi {

/// Every backend checks indirect arguments the same way before recording (VCH-071), so a call that works on one
/// works on all: `count` records of `record_size` bytes, `stride` apart from `offset`, inside an indirect buffer.
inline void check_indirect_args(const BufferDesc* args, std::uint64_t offset, std::uint32_t count, std::uint32_t stride,
                                std::uint32_t record_size, std::string_view call) {
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
inline void check_index_buffer(const BufferDesc* indices, std::string_view call) {
    VEACH_ASSERT(indices != nullptr, "{}: invalid index buffer", call);
    VEACH_ASSERT(has(indices->usage, BufferUsage::index), "{}: buffer '{}' was not created with BufferUsage::index",
                 call, indices->debug_name);
}

}  // namespace veach::rhi
