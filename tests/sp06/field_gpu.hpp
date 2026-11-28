#pragma once

// Spike SP-06 (VCH-082): the field graph on the GPU, two ways, through the render graph: the interpreter walking the
// op list (shaders/sp06_field_interpreter.slang) and the hand-written kernel (shaders/sp06_field_handwritten.slang).

#include <veach/procgen/field_graph.hpp>

#include <veach/core/error.hpp>
#include <veach/rhi/rhi.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace veach::sp06 {

struct GpuRun {
    std::vector<float> interpreted;  ///< resolution^2, rows top to bottom (from the last frame)
    std::vector<float> handwritten;
    std::vector<double> interpreted_ms;  ///< render-graph GPU time of each frame's pass
    std::vector<double> handwritten_ms;
    std::string timing_unavailable;  ///< why the times are empty, if they are
};

/// Runs both kernels over `region` once per frame for `frames` frames (alternating which goes first) and reads the
/// last frame's results back. The interpreter walks engine/procgen's op list (veach::procgen::FieldProgram::words).
[[nodiscard]] Result<GpuRun> run_gpu(rhi::Device& device, const procgen::FieldProgram& program,
                                     const procgen::FaceRegion& region, std::uint32_t frames);

/// The terrain graph (tests/procgen/terrain.field.json) compiled for a 6,000 km body: the program the hand-written
/// kernel bakes in for seed 42.
[[nodiscard]] Result<procgen::FieldProgram> terrain_program(std::uint64_t seed = 42);

}  // namespace veach::sp06
