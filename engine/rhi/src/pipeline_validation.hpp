#pragma once

#include <veach/rhi/rhi.hpp>

#include <format>

namespace veach::rhi {

/// Every backend runs this before creating a graphics pipeline, so a pipeline that works on one works on all.
inline Result<> validate_graphics_pipeline_desc(const GraphicsPipelineDesc& desc) {
    if (desc.depth_format != Format::undefined && desc.depth_format != Format::depth32_float) {
        return make_error(ErrorCode::rhi_invalid_pipeline,
                          std::format("graphics pipeline '{}': {} is not a depth format", desc.debug_name,
                                      to_string(desc.depth_format)),
                          "depth_format names the render pass's depth attachment: depth32_float, or undefined.");
    }
    if ((desc.depth_test || desc.depth_write) && desc.depth_format == Format::undefined) {
        return make_error(
            ErrorCode::rhi_invalid_pipeline,
            std::format("graphics pipeline '{}' tests or writes depth but has no depth_format", desc.debug_name),
            "Set depth_format to the depth attachment's format (depth32_float) and give the render "
            "pass that attachment (PassBuilder::depth_target).");
    }
    if (desc.depth_bias != DepthBias{} && desc.depth_format == Format::undefined) {
        return make_error(ErrorCode::rhi_invalid_pipeline,
                          std::format("graphics pipeline '{}' has a depth bias but no depth_format", desc.debug_name),
                          "Depth bias moves the depth a pass tests and writes: give the pipeline a depth_format.");
    }
    for (std::size_t i = 0; i < desc.color_formats.size(); ++i) {
        const Format format = desc.color_formats.at(i);
        if (desc.blend.at(i) != BlendMode::opaque && (format == Format::undefined || is_integer(format))) {
            return make_error(ErrorCode::rhi_invalid_pipeline,
                              std::format("graphics pipeline '{}': color target {} ({}) cannot blend", desc.debug_name,
                                          i, to_string(format)),
                              "Blending needs a float, unorm or sRGB color target; integer targets are written as is.");
        }
    }
    return {};
}

/// Every backend runs this before creating a sampler.
inline Result<> validate_sampler_desc(const SamplerDesc& desc) {
    if (desc.max_anisotropy == 0 || desc.max_anisotropy > max_anisotropy_limit) {
        return make_error(ErrorCode::rhi_invalid_sampler,
                          std::format("a sampler with max_anisotropy {}", desc.max_anisotropy),
                          "max_anisotropy is 1 (off) to 16; terrain uses rhi::terrain_anisotropy (8).");
    }
    if (desc.max_anisotropy > 1 && desc.filter != Filter::linear) {
        return make_error(ErrorCode::rhi_invalid_sampler, "an anisotropic sampler with nearest filtering",
                          "Anisotropic filtering is a linear filter: use Filter::linear.");
    }
    return {};
}

}  // namespace veach::rhi
