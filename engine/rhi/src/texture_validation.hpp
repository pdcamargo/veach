#pragma once

#include <veach/rhi/rhi.hpp>

#include <format>

namespace veach::rhi {

/// Every backend runs this before creating a texture, so a texture that works on one works on all.
inline Result<> validate_texture_desc(const TextureDesc& desc) {
    const auto invalid = [&](std::string why, std::string_view hint) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': a {}x{} {} {} texture {}", desc.debug_name, desc.width, desc.height,
                                      to_string(desc.type), to_string(desc.format), why),
                          std::string(hint));
    };
    // D3D12 copies whole blocks into every mip but refuses an 8x8 copy into a 6x6 top level (VCH-051).
    if (is_block_compressed(desc.format) && (desc.width % 4 != 0 || desc.height % 4 != 0)) {
        return invalid("has a size that is not a multiple of 4",
                       "The texture cooker rounds sizes up to a multiple of 4; do the same for textures made in code.");
    }
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.layers == 0) {
        return invalid("has a zero size", "Every texture is at least 1x1x1 with one layer.");
    }
    std::uint32_t largest = std::max(desc.width, desc.height);
    if (desc.type == TextureType::d3) {
        largest = std::max(largest, desc.depth);
    }
    std::uint32_t full_chain = 1;
    while ((largest >> full_chain) > 0) {
        ++full_chain;
    }
    if (desc.mip_levels > full_chain) {
        return invalid(std::format("asks for {} mips; its full chain has {}", desc.mip_levels, full_chain),
                       "A mip chain ends at 1x1x1.");
    }
    const bool targets = has(desc.usage, TextureUsage::color_target) || has(desc.usage, TextureUsage::depth_target);
    switch (desc.type) {
        case TextureType::d2:
            if (desc.layers != 1 || desc.depth != 1) {
                return invalid(std::format("has {} layers and a depth of {}", desc.layers, desc.depth),
                               "Use TextureType::d2_array for layers and TextureType::d3 for depth.");
            }
            break;
        case TextureType::d2_array:
            if (desc.depth != 1) {
                return invalid(std::format("has a depth of {}", desc.depth),
                               "Array layers are `layers`; `depth` is for TextureType::d3.");
            }
            break;
        case TextureType::d3:
            if (desc.layers != 1) {
                return invalid(std::format("has {} layers", desc.layers),
                               "3D textures have no layers: their slices are `depth`.");
            }
            if (is_block_compressed(desc.format) || desc.format == Format::depth32_float ||
                has(desc.usage, TextureUsage::depth_target)) {
                return invalid("cannot be 3D", "3D textures hold color data, uncompressed: no BC7, no depth.");
            }
            break;
        case TextureType::cube:
        case TextureType::cube_array:
            if (desc.width != desc.height || desc.depth != 1) {
                return invalid("is not a square with a depth of 1", "Cube faces are square.");
            }
            if (desc.type == TextureType::cube ? desc.layers != 6 : desc.layers % 6 != 0) {
                return invalid(std::format("has {} layers", desc.layers),
                               "A cube has 6 layers (+X, -X, +Y, -Y, +Z, -Z); a cube array 6 per cube.");
            }
            break;
    }
    if (targets && is_block_compressed(desc.format)) {
        return invalid("cannot be a render target", "Block-compressed textures are only sampled and copied into.");
    }
    if (has(desc.usage, TextureUsage::storage) &&
        (is_block_compressed(desc.format) || desc.format == Format::depth32_float || is_srgb(desc.format))) {
        return invalid("cannot be a storage texture",
                       "Shaders write uncompressed linear formats (rgba8_unorm, rgba16_float...), not BC7, sRGB or "
                       "depth.");
    }
    if (desc.sample_count != 1 && desc.sample_count != 2 && desc.sample_count != 4 && desc.sample_count != 8) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': sample_count {}", desc.debug_name, desc.sample_count),
                          "A texture has 1, 2, 4 or 8 samples.");
    }
    if (desc.sample_count > 1 &&
        (desc.type != TextureType::d2 || desc.mip_levels > 1 || is_block_compressed(desc.format) ||
         has(desc.usage, TextureUsage::sampled) || has(desc.usage, TextureUsage::storage) ||
         has(desc.usage, TextureUsage::copy_src) || has(desc.usage, TextureUsage::copy_dst))) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': a multisampled texture that is not only an attachment", desc.debug_name),
                          "Multisampled textures (spike SP-16) live in tile memory: one mip, usage color_target or "
                          "depth_target only, read through a resolve (ColorAttachment::resolve).");
    }
    return {};
}

/// Initial data: one span per mip, each holding at least mip_bytes (every layer and slice of the level).
inline Result<> validate_mip_data(const TextureDesc& desc, std::span<const std::span<const std::byte>> mips) {
    for (std::uint32_t level = 0; level < mips.size(); ++level) {
        if (mips[level].size() < mip_bytes(desc, level)) {
            return make_error(ErrorCode::rhi_invalid_texture,
                              std::format("'{}': mip {} has {} bytes of data, not {}", desc.debug_name, level,
                                          mips[level].size(), mip_bytes(desc, level)),
                              "Each mip's data holds every layer and depth slice, tightly packed (rhi::mip_bytes).");
        }
    }
    return {};
}

/// A buffer-to-texture copy's source layout: aligned for D3D12, rows long enough, inside the buffer.
inline bool valid_copy_layout(Format format, const TextureRegion& region, std::uint64_t offset, std::uint32_t row_pitch,
                              std::uint64_t buffer_size) {
    const std::uint64_t image = std::uint64_t{row_pitch} * row_count(format, region.height) * region.depth;
    const std::uint64_t stride = texture_copy_layer_stride(format, region, row_pitch);
    return offset % texture_copy_offset_alignment == 0 && row_pitch % texture_copy_row_alignment == 0 &&
           row_pitch >= row_bytes(format, region.width) &&
           offset + stride * (region.layer_count - 1) + image <= buffer_size;
}

/// Every backend runs this for a view before creating it: the range lies in the texture, storage views see one mip.
inline Result<TextureRange> resolve_range(const TextureDesc& desc, View view, const TextureRange& range) {
    const std::uint32_t mips = std::max(1u, desc.mip_levels);
    const std::uint32_t layers = layer_count(desc);
    TextureRange out = range;
    if (out.base_mip >= mips || out.base_layer >= layers) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': a view from mip {} and layer {} of a texture with {} mips and {} layers",
                                      desc.debug_name, out.base_mip, out.base_layer, mips, layers),
                          "TextureRange counts mips and layers from 0.");
    }
    out.mip_count = view == View::storage ? 1 : std::min(out.mip_count, mips - out.base_mip);
    out.layer_count = std::min(out.layer_count, layers - out.base_layer);
    if (out.mip_count == 0 || out.layer_count == 0) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': a view of no mips or no layers", desc.debug_name),
                          "Leave mip_count and layer_count at rhi::remaining for the rest of the texture.");
    }
    return out;
}

/// The type a view of `range` has: the texture's, except that a part of a cube (or any storage view of one) is a 2D
/// array, as is a storage view of a cube: shaders write cube faces as layers (RWTexture2DArray).
inline TextureType view_type(const TextureDesc& desc, View view, const TextureRange& range) {
    const bool cube = desc.type == TextureType::cube || desc.type == TextureType::cube_array;
    if (cube && (view == View::storage || range.base_layer != 0 || range.layer_count != desc.layers)) {
        return TextureType::d2_array;
    }
    return desc.type;
}

/// Checks a render pass attachment against its texture.
inline bool valid_target(const TextureDesc& desc, const TargetSlice& slice) {
    const std::uint32_t layers = desc.type == TextureType::d3 ? depth_of_mip(desc, slice.mip) : layer_count(desc);
    return slice.mip < std::max(1u, desc.mip_levels) && slice.layer < layers;
}

}  // namespace veach::rhi
