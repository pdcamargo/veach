// Spike SP-06 (VCH-082): the GPU half of docs/research/spikes/sp-06-field-graph.md. The compiler and the canonical CPU
// interpreter moved to engine/procgen (VCH-112), with SP-06's committed hashes, in tests/procgen_tests.cpp; what stays
// here is the GPU interpreter and the hand-written kernel, which agree with the engine's interpreter. The GPU
// evaluator's home is engine/render (ADR 0065), which this prototypes.

#include "sp06/field_gpu.hpp"
#include "support/gpu_device.hpp"

#include <veach/procgen/field_graph.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace veach;

namespace {

float max_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }
    return worst;
}

}  // namespace

TEST_CASE("[sp06.AC3] the GPU interpreter and the hand-written kernel agree with the CPU interpreter within 1e-5") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    auto program = sp06::terrain_program();
    REQUIRE_MESSAGE(program.has_value(), (program ? std::string() : program.error().message));
    procgen::FaceRegion region;
    region.resolution = 256;
    std::vector<float> cpu(region.size());
    procgen::evaluate(*program, region, cpu);
    auto gpu = sp06::run_gpu(*device, *program, region, 2);
    REQUIRE_MESSAGE(gpu.has_value(), (gpu ? std::string() : gpu.error().message));
    const float interpreted = max_abs_difference(gpu->interpreted, cpu);
    const float handwritten = max_abs_difference(gpu->handwritten, cpu);
    MESSAGE("max |GPU interpreter - CPU| = ", interpreted, ", max |hand-written - CPU| = ", handwritten);
    CHECK(interpreted < 1e-5f);
    CHECK(handwritten < 1e-5f);
    CHECK(max_abs_difference(gpu->interpreted, gpu->handwritten) < 1e-5f);
    CHECK((gpu->interpreted_ms.size() == 2 || !gpu->timing_unavailable.empty()));
}
