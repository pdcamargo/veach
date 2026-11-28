// Spike SP-12 (VCH-088): brush edits replay to identical chunk hashes with any worker count and chunk order, Jolt
// colliders cooked from the chunks install deterministically, and a ball falls into the dug pit
// (docs/research/spikes/sp-12-dig-collide.md). The hashes were committed from macOS arm64; Windows x64 CI checks them:
// the edit path and the mesher are canonical (ADR 0043), Jolt is built cross-platform deterministic (ADR 0058).

#include <doctest/doctest.h>

#include "sp12/sp12_scenario.hpp"

#include <veach/core/hash.hpp>

#include <cstdint>
#include <format>
#include <iterator>

namespace {

using namespace veach;
using namespace veach::sp12;
using voxel::BrushKind;
using voxel::BrushOp;

struct Runs {
    ScenarioResult one, four, reversed, control;
};

/// The scenario on 1 worker, on 4, on 4 with every op applied to its chunks in reverse key order, and without the
/// dig. Run once (sampling dominates a Debug build).
const Runs& runs() {
    static const Runs r = [] {
        Runs out;
        ScenarioConfig config;
        out.one = run_scenario(config);
        config.workers = 4;
        out.four = run_scenario(config);
        config.reverse = true;
        out.reversed = run_scenario(config);
        config = {};
        config.workers = 4;
        config.dig = false;
        out.control = run_scenario(config);
        return out;
    }();
    return r;
}

std::string hex(std::uint64_t v) {
    return std::format("0x{:016x}", v);
}

// Regenerate only on purpose: a different value means the field, the brush arithmetic, the mesher, Jolt or its build
// flags changed. The INFO lines of a failing run print the new values.
constexpr std::uint64_t base_samples_expected = 0xace58c96ce261f0dULL;
constexpr std::uint64_t base_mesh_expected = 0x796b6be1210887a9ULL;
constexpr std::uint64_t samples_expected = 0xd0163ab3d7069434ULL;
constexpr std::uint64_t mesh_expected = 0x1be19fd52b71cdadULL;
constexpr std::uint64_t colliders_expected = 0x8927396d27667325ULL;
constexpr std::uint64_t install_expected = 0x8097f58466b787c5ULL;
constexpr std::uint64_t ball_expected[std::size(snapshot_steps)] = {0xd0ea06ffda4a1993ULL, 0x14904caff3ab6ea5ULL,
                                                                    0x5b7f8941f7bd71b4ULL, 0x2fe0ad520bcd7587ULL};
constexpr std::uint64_t chain_expected = 0x25dd581926b6d60fULL;

}  // namespace

TEST_CASE("[sp12.AC1] isqrt is exact and brush ops don't commute") {
    for (std::uint64_t v : {0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 99ULL, 100ULL, 101ULL, 0xFFFFFFFFULL, 0x100000000ULL}) {
        const std::uint64_t r = voxel::isqrt(v);
        CHECK(r * r <= v);
        CHECK((r + 1) * (r + 1) > v);
    }
    CHECK(voxel::isqrt(~0ULL) == 0xFFFFFFFFULL);
    CHECK(voxel::isqrt(0xFFFFFFFE00000001ULL) == 0xFFFFFFFFULL);  // (2^32 − 1)²
    CHECK(voxel::isqrt(0xFFFFFFFE00000000ULL) == 0xFFFFFFFEULL);

    // An op near a chunk corner reaches the neighbors whose apron it touches.
    BrushOp op;
    op.center_mm = {16'000, 16'000, 16'000};  // the corner shared by 8 LOD0 chunks
    op.radius_mm = 1'000;
    CHECK(voxel::touched_chunks(op).size() == 8);
    op.center_mm = {8'000, 8'000, 8'000};  // the middle of chunk (0, 0, 0), 8 m from every face
    CHECK(voxel::touched_chunks(op).size() == 1);

    // Subtract then add is not add then subtract: the log is ordered.
    voxel::ChunkSamples a;
    a.key = {};
    a.grid.assign(static_cast<std::size_t>(voxel::ChunkSamples::grid_n) * voxel::ChunkSamples::grid_n *
                      voxel::ChunkSamples::grid_n,
                  voxel::Sample{-4096, 2});  // solid, 4 m deep
    voxel::ChunkSamples b = a;
    voxel::OverrideMask ma, mb;
    BrushOp dig;
    dig.kind = BrushKind::subtract;
    dig.center_mm = {8'000, 8'000, 8'000};
    dig.radius_mm = 3'000;
    BrushOp fill = dig;
    fill.kind = BrushKind::add;
    fill.radius_mm = 2'000;
    fill.material = 3;
    voxel::apply_brush(dig, a, ma);
    voxel::apply_brush(fill, a, ma);
    voxel::apply_brush(fill, b, mb);
    voxel::apply_brush(dig, b, mb);
    CHECK(voxel::samples_hash(a) != voxel::samples_hash(b));
    // At the center: dug then filled is solid, 2 m inside the fill; filled then dug is air, 3 m from the wall.
    CHECK(a.at(16, 16, 16).distance == -2048);
    CHECK(a.at(16, 16, 16).material == 3);
    CHECK(b.at(16, 16, 16).distance == 3072);
    CHECK(a.has_surface);
    // The mask marks exactly what changed; samples beyond the brush's reach keep their value.
    CHECK(ma[voxel::ChunkSamples::grid_index(16, 16, 16)] == 1);
    CHECK(ma[voxel::ChunkSamples::grid_index(0, 0, 0)] == 0);
    CHECK(a.at(0, 0, 0).distance == -4096);
}

TEST_CASE("[sp12.AC2] the edit log replays to the committed chunk hashes, on 1 and 4 workers, in any chunk order") {
    const Runs& r = runs();
    for (const ScenarioResult* run : {&r.one, &r.four, &r.reversed}) {
        INFO(std::format("base samples {} mesh {}; edited samples {} mesh {}", hex(run->base_samples_hash),
                         hex(run->base_mesh_hash), hex(run->samples_hash), hex(run->mesh_hash)));
        CHECK(run->base_samples_hash == base_samples_expected);
        CHECK(run->base_mesh_hash == base_mesh_expected);
        CHECK(run->samples_hash == samples_expected);
        CHECK(run->mesh_hash == mesh_expected);
        CHECK(run->outside == 0);
        CHECK(run->mismatches == 0);  // neighbors hold equal samples at shared positions
        CHECK(run->open_edges == 0);  // the dug terrain is watertight
        // As in SP-08, a few triangles fold at thin features (the mound's rim): 2 of 98,504 here.
        CHECK(static_cast<double>(run->wound_out) > 0.9999 * static_cast<double>(run->triangles));
    }
    CHECK(r.one.changed >= 4);
    CHECK(r.one.materialized >= 1);  // the cavity reached a chunk the bounds had proven solid
    CHECK(r.control.samples_hash == r.one.base_samples_hash);
}

TEST_CASE("[sp12.AC3] colliders cooked from the chunks install with the committed hashes") {
    const Runs& r = runs();
    for (const ScenarioResult* run : {&r.one, &r.four, &r.reversed}) {
        INFO(std::format("colliders {} install {}", hex(run->colliders_hash), hex(run->install_hash)));
        CHECK(run->colliders_hash == colliders_expected);
        CHECK(run->install_hash == install_expected);
        CHECK(run->terrain_bodies == run->meshed);
    }
}

TEST_CASE("[sp12.AC4] a ball on the dig site falls into the pit and comes to rest below the original surface") {
    const Runs& r = runs();
    for (const ScenarioResult* run : {&r.one, &r.four, &r.reversed}) {
        REQUIRE(run->step_hashes.size() == static_cast<std::size_t>(snapshot_steps[std::size(snapshot_steps) - 1]));
        for (std::size_t i = 0; i < std::size(snapshot_steps); ++i) {
            const auto step = static_cast<std::size_t>(snapshot_steps[i]);
            INFO(std::format("ball after step {}: {}", step, hex(run->step_hashes[step - 1])));
            CHECK(run->step_hashes[step - 1] == ball_expected[i]);
        }
        INFO(std::format("chain {}", hex(run->chain)));
        CHECK(run->chain == chain_expected);
        CHECK(run->jolt_state == r.one.jolt_state);
        INFO(
            std::format("before the pit: {} m; before the sump: {} m ({}); at the end: {} m deep, {} m off the "
                        "axis, {} m/s",
                        run->batches[0].depth_before, run->batches[1].depth_before,
                        run->batches[1].asleep_before ? "asleep" : "awake", run->depth, run->lateral, run->speed));
        // On the ground before the dig: the ball's center is about its radius (0.4 m) above the surface.
        CHECK(run->batches[0].depth_before > -0.2);
        CHECK(run->batches[0].depth_before < 0.6);
        // At rest in the pit (3 m deep) before the sump: asleep, so the install must wake it.
        CHECK(run->batches[1].depth_before < -2.0);
        CHECK(run->batches[1].asleep_before);
        // In the sump (4.4 m deep), at rest.
        CHECK(run->depth < -3.5);
        CHECK(run->lateral < 1.5);
        CHECK(run->speed < 0.01);
    }
    INFO(std::format("control: {} m, {} m/s", r.control.depth, r.control.speed));
    CHECK(r.control.depth > -0.2);  // without the dig the ball stays on the ground
    CHECK(r.control.chain != r.one.chain);
}
