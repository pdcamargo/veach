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
    return {};
}

}  // namespace veach::rhi
