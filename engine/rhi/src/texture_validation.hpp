#pragma once

#include <veach/rhi/rhi.hpp>

#include <format>

namespace veach::rhi {

/// Every backend runs this before creating a texture, so a texture that works on one works on all.
inline Result<> validate_texture_desc(const TextureDesc& desc) {
    // D3D12 copies whole blocks into every mip but refuses an 8x8 copy into a 6x6 top level (VCH-051).
    if (is_block_compressed(desc.format) && (desc.width % 4 != 0 || desc.height % 4 != 0)) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': a {}x{} {} texture; block-compressed sizes must be multiples of 4",
                                      desc.debug_name, desc.width, desc.height, to_string(desc.format)),
                          "The texture cooker rounds sizes up to a multiple of 4; do the same for textures made "
                          "in code.");
    }
    return {};
}

}  // namespace veach::rhi
