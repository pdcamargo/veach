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
    if (desc.sample_count != 1 && desc.sample_count != 2 && desc.sample_count != 4 && desc.sample_count != 8) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': sample_count {}", desc.debug_name, desc.sample_count),
                          "A texture has 1, 2, 4 or 8 samples.");
    }
    if (desc.sample_count > 1 && (desc.mip_levels > 1 || is_block_compressed(desc.format) ||
                                  has(desc.usage, TextureUsage::sampled) || has(desc.usage, TextureUsage::storage) ||
                                  has(desc.usage, TextureUsage::copy_src) || has(desc.usage, TextureUsage::copy_dst))) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': a multisampled texture that is not only an attachment", desc.debug_name),
                          "Multisampled textures (spike SP-16) live in tile memory: one mip, usage color_target or "
                          "depth_target only, read through a resolve (ColorAttachment::resolve).");
    }
    return {};
}

}  // namespace veach::rhi
