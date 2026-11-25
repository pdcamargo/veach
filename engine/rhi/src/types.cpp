#include <veach/rhi/types.hpp>

namespace veach::rhi {

std::uint32_t bytes_per_pixel(Format format) {
    switch (format) {
        case Format::rgba8_unorm:
        case Format::bgra8_unorm:
        case Format::r32_float:
        case Format::depth32_float: return 4;
        case Format::rgba16_float: return 8;
        case Format::rgb32_float: return 12;
        case Format::rgba32_float: return 16;
        case Format::undefined: return 0;
    }
    return 0;
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
    }
    return "?";
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
