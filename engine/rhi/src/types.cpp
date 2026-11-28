#include <veach/rhi/types.hpp>

#include <algorithm>
#include <format>

namespace veach::rhi {

std::uint32_t bytes_per_pixel(Format format) {
    switch (format) {
        case Format::rgba8_unorm:
        case Format::rgba8_srgb:
        case Format::bgra8_unorm:
        case Format::r32_float:
        case Format::r32_uint:
        case Format::depth32_float:
        case Format::rg16_float: return 4;
        case Format::r8_unorm: return 1;
        case Format::rg8_unorm:
        case Format::r16_float: return 2;
        case Format::rgba16_float:
        case Format::rg32_uint: return 8;
        case Format::rgb32_float: return 12;
        case Format::rgba32_float: return 16;
        case Format::bc7_unorm:
        case Format::bc7_srgb:
        case Format::undefined: return 0;
    }
    return 0;
}

bool is_block_compressed(Format format) {
    return format == Format::bc7_unorm || format == Format::bc7_srgb;
}

bool is_srgb(Format format) {
    return format == Format::rgba8_srgb || format == Format::bc7_srgb;
}

bool is_integer(Format format) {
    return format == Format::r32_uint || format == Format::rg32_uint;
}

std::uint64_t row_bytes(Format format, std::uint32_t width) {
    if (is_block_compressed(format)) {
        return std::uint64_t{(width + 3) / 4} * 16;
    }
    return std::uint64_t{width} * bytes_per_pixel(format);
}

std::uint32_t row_count(Format format, std::uint32_t height) {
    return is_block_compressed(format) ? (height + 3) / 4 : height;
}

std::string_view to_string(Format format) {
    switch (format) {
        case Format::undefined: return "undefined";
        case Format::rgba8_unorm: return "rgba8_unorm";
        case Format::bgra8_unorm: return "bgra8_unorm";
        case Format::rgba16_float: return "rgba16_float";
        case Format::rgba32_float: return "rgba32_float";
        case Format::r32_float: return "r32_float";
        case Format::rgb32_float: return "rgb32_float";
        case Format::depth32_float: return "depth32_float";
        case Format::rgba8_srgb: return "rgba8_srgb";
        case Format::bc7_unorm: return "bc7_unorm";
        case Format::bc7_srgb: return "bc7_srgb";
        case Format::r32_uint: return "r32_uint";
        case Format::rg32_uint: return "rg32_uint";
        case Format::r8_unorm: return "r8_unorm";
        case Format::rg8_unorm: return "rg8_unorm";
        case Format::r16_float: return "r16_float";
        case Format::rg16_float: return "rg16_float";
    }
    return "?";
}

std::uint32_t layer_count(const TextureDesc& desc) {
    return desc.type == TextureType::d2 || desc.type == TextureType::d3 ? 1 : desc.layers;
}

std::uint32_t depth_of_mip(const TextureDesc& desc, std::uint32_t level) {
    return desc.type == TextureType::d3 ? mip_size(desc.depth, level) : 1;
}

std::uint64_t mip_bytes(const TextureDesc& desc, std::uint32_t level) {
    return row_bytes(desc.format, mip_size(desc.width, level)) * row_count(desc.format, mip_size(desc.height, level)) *
           depth_of_mip(desc, level) * layer_count(desc);
}

std::string_view to_string(TextureType type) {
    switch (type) {
        case TextureType::d2: return "d2";
        case TextureType::d2_array: return "d2_array";
        case TextureType::d3: return "d3";
        case TextureType::cube: return "cube";
        case TextureType::cube_array: return "cube_array";
    }
    return "?";
}

std::uint32_t texture_copy_pitch(Format format, std::uint32_t width) {
    const std::uint64_t row = row_bytes(format, width);
    return static_cast<std::uint32_t>((row + texture_copy_row_alignment - 1) / texture_copy_row_alignment *
                                      texture_copy_row_alignment);
}

std::uint64_t texture_copy_layer_stride(Format format, const TextureRegion& region, std::uint32_t row_pitch) {
    const std::uint64_t bytes =
        std::uint64_t{row_pitch} * row_count(format, region.height) * std::max(1u, region.depth);
    return (bytes + texture_copy_offset_alignment - 1) / texture_copy_offset_alignment * texture_copy_offset_alignment;
}

Result<TextureRegion> resolve_region(const TextureDesc& desc, const TextureRegion& region) {
    const auto fail = [&](std::string why) {
        return make_error(ErrorCode::rhi_invalid_texture,
                          std::format("'{}': region (mip {}, layers {}+{}, {},{},{} size {}x{}x{}) {}", desc.debug_name,
                                      region.mip, region.base_layer, region.layer_count, region.x, region.y, region.z,
                                      region.width, region.height, region.depth, why),
                          "Copy regions lie inside one mip of the texture; block-compressed regions start on 4x4 "
                          "blocks and end on them or at the mip's edge.");
    };
    if (region.mip >= std::max(1u, desc.mip_levels)) {
        return fail(std::format("names a mip the texture does not have ({} levels)", std::max(1u, desc.mip_levels)));
    }
    const std::uint32_t w = mip_size(desc.width, region.mip);
    const std::uint32_t h = mip_size(desc.height, region.mip);
    const std::uint32_t d = depth_of_mip(desc, region.mip);
    TextureRegion out = region;
    if (out.x >= w || out.y >= h || out.z >= d) {
        return fail(std::format("starts outside the {}x{}x{} mip", w, h, d));
    }
    out.width = out.width == 0 ? w - out.x : out.width;
    out.height = out.height == 0 ? h - out.y : out.height;
    out.depth = out.depth == 0 ? d - out.z : out.depth;
    if (std::uint64_t{out.x} + out.width > w || std::uint64_t{out.y} + out.height > h ||
        std::uint64_t{out.z} + out.depth > d) {
        return fail(std::format("does not fit in the {}x{}x{} mip", w, h, d));
    }
    if (out.layer_count == 0 || std::uint64_t{out.base_layer} + out.layer_count > layer_count(desc)) {
        return fail(std::format("does not fit in the texture's {} layers", layer_count(desc)));
    }
    if (is_block_compressed(desc.format)) {
        const bool starts = out.x % 4 == 0 && out.y % 4 == 0;
        const bool ends = ((out.x + out.width) % 4 == 0 || out.x + out.width == w) &&
                          ((out.y + out.height) % 4 == 0 || out.y + out.height == h);
        if (!starts || !ends) {
            return fail("cuts a 4x4 block");
        }
    }
    return out;
}

std::string_view to_string(ResourceState state) {
    switch (state) {
        case ResourceState::undefined: return "undefined";
        case ResourceState::shader_read: return "shader_read";
        case ResourceState::shader_write: return "shader_write";
        case ResourceState::color_target: return "color_target";
        case ResourceState::depth_write: return "depth_write";
        case ResourceState::copy_src: return "copy_src";
        case ResourceState::copy_dst: return "copy_dst";
        case ResourceState::accel_build: return "accel_build";
        case ResourceState::accel_read: return "accel_read";
        case ResourceState::present: return "present";
    }
    return "?";
}

}  // namespace veach::rhi
