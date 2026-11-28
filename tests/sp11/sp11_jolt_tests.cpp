// Spike SP-11 (VCH-087): Jolt built with JPH_DOUBLE_PRECISION and JPH_CROSS_PLATFORM_DETERMINISTIC gives the same
// state hash on the macOS gate and Windows CI, with 1 and N workers (docs/research/spikes/sp-11-jolt.md,
// docs/adr/0058-physics-jolt.md). The hashes below were committed from macOS arm64; Windows x64 CI checks them.

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <iterator>

#include "sp11/sp11_scene.hpp"

namespace {

using veach::sp11::SceneConfig;
using veach::sp11::SceneResult;

struct Expected {
    std::uint64_t snapshots[std::size(veach::sp11::snapshot_steps)];
    std::uint64_t chain;
};

// Regenerate only on purpose: a different value means Jolt, its build flags or the scene changed. The INFO lines of
// a failing run print the new values.
constexpr Expected origin_expected = {
    {0x90f546172584bb1fULL, 0x89a2820b9fb3be9eULL, 0x79d47a6ec4279409ULL, 0x87bdb4156d528362ULL},
    0x171ce203da644de9ULL};
constexpr Expected offset_expected = {
    {0x82536d1fc1f7bec7ULL, 0xd756aa6a7898d1f0ULL, 0x8e0eb4048226ac7fULL, 0x364bc02eeecdeb99ULL},
    0x48bceab5bca8cea8ULL};

SceneConfig scene(int workers, bool offset) {
    SceneConfig config;
    config.bodies = 200;
    config.steps = 600;
    config.workers = workers;
    if (offset) {
        config.offset[0] = 3'822'600.0;  // 6,371 km from the origin (a 3-4-5 triangle: 0.6 and 0.8 of it).
        config.offset[1] = 5'096'800.0;
    }
    return config;
}

void check_committed(const SceneResult& r, const Expected& expected) {
    REQUIRE(r.step_hashes.size() == 600);
    CHECK(r.update_errors == 0);
    for (std::size_t i = 0; i < std::size(veach::sp11::snapshot_steps); ++i) {
        const auto step = static_cast<std::size_t>(veach::sp11::snapshot_steps[i]);
        INFO(std::format("state hash after step {}: 0x{:016x}", step, r.step_hashes[step - 1]));
        CHECK(r.step_hashes[step - 1] == expected.snapshots[i]);
    }
    INFO(std::format("chained hash of all 600 steps: 0x{:016x}", r.chain));
    CHECK(r.chain == expected.chain);
}

void check_same_run(const SceneResult& a, const SceneResult& b) {
    REQUIRE(a.step_hashes.size() == b.step_hashes.size());
    std::size_t first_difference = a.step_hashes.size();
    for (std::size_t i = 0; i < a.step_hashes.size(); ++i) {
        if (a.step_hashes[i] != b.step_hashes[i]) {
            first_difference = i;
            break;
        }
    }
    INFO(std::format("first differing step: {}", first_difference + 1));
    CHECK(first_difference == a.step_hashes.size());
    CHECK(a.jolt_state == b.jolt_state);
}

/// The pile really collided and settled in the bowl: every body is inside it and above the ground.
void check_settled(const SceneResult& r) {
    for (std::size_t b = 0; b + 2 < r.positions.size(); b += 3) {
        const double x = r.positions[b], y = r.positions[b + 1], z = r.positions[b + 2];
        INFO(std::format("body {} at ({}, {}, {})", b / 3, x, y, z));
        CHECK(x * x + z * z < 31.0 * 31.0);
        CHECK(y > -0.5);
        CHECK(y < 16.0);
    }
}

}  // namespace

TEST_CASE("[sp11.AC1] a Jolt pile at the origin gives the committed state hashes, 1 and 4 workers") {
    const SceneResult single = veach::sp11::run_scene(scene(0, false));
    check_committed(single, origin_expected);
    check_settled(single);
    const SceneResult pool = veach::sp11::run_scene(scene(4, false));
    check_same_run(single, pool);
}

TEST_CASE("[sp11.AC2] the same pile 6,371 km from the origin gives its committed hashes, 1 and 4 workers") {
    const SceneResult single = veach::sp11::run_scene(scene(0, true));
    check_committed(single, offset_expected);
    check_settled(single);
    const SceneResult pool = veach::sp11::run_scene(scene(4, true));
    check_same_run(single, pool);
}
