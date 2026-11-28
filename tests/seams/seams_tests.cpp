// The seams sweep (docs/research/spikes/seams-sweep.md): terrain, digging, walking and landing at the octant planes,
// the poles, the cube-corner directions and chunk corners, and the cube-mapped procgen inputs across face edges. The
// hashes were committed from macOS arm64; Windows x64 CI checks them (the field, the mesher and the edit path are
// canonical, ADR 0043; Jolt is built cross-platform deterministic, ADR 0058).

#include <doctest/doctest.h>

#include "seams/seams.hpp"
#include "seams/seams_cubemap.hpp"
#include "sp06/field_gpu.hpp"
#include "sp13/sp13_sim.hpp"

#include <veach/core/hash.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace {

using namespace veach;
using namespace veach::seams;

std::string hex(std::uint64_t v) {
    return std::format("0x{:016x}", v);
}

/// Each part runs once per process, in the test case that needs it: ctest runs the test cases in parallel processes,
/// and sampling dominates a Debug build.
struct LodRun {
    SiteSpec spec;
    Site site;
    LodResult lod;
    std::vector<std::pair<int, Continuity>> continuity;  ///< per octant plane the site lies on
};

const std::vector<LodRun>& lod_runs() {
    static const std::vector<LodRun> runs = [] {
        std::vector<LodRun> out;
        for (const SiteSpec& spec : sweep_sites()) {
            LodRun run{spec, make_site(spec), {}, {}};
            LodConfig lod;
            lod.site = run.site;
            lod.root_level = 2;  // roots of 64 m: LOD0, the ring to level 1 and, at most sites, to level 2
            run.lod = run_lod(lod);
            for (int a = 0; a < 3; ++a) {
                if ((octant_planes(run.site) >> a & 1u) != 0) {
                    run.continuity.emplace_back(a, check_continuity(run.site, a));
                }
            }
            out.push_back(std::move(run));
        }
        return out;
    }();
    return runs;
}

const std::vector<sp12::ScenarioResult>& dig_runs() {
    static const std::vector<sp12::ScenarioResult> runs = [] {
        std::vector<sp12::ScenarioResult> out;
        for (const SiteSpec& spec : sweep_sites()) {
            sp12::ScenarioConfig dig;
            dig.site = make_site(spec);
            dig.workers = 0;
            out.push_back(sp12::run_scenario(dig));
        }
        return out;
    }();
    return runs;
}

struct Walks {
    std::vector<WalkConfig> walks;
    std::vector<WalkResult> walked;
    /// The border-edge comparison on three walks: chunk colliders, one merged collider, adjacency rings.
    std::vector<std::size_t> compared;
    std::vector<WalkResult> merged, adjacency;
};

const Walks& walk_runs() {
    static const Walks runs = [] {
        Walks out;
        out.walks = sweep_walks();
        for (WalkConfig w : out.walks) {
            w.legs = 2;
            out.walked.push_back(run_walk(w));
        }
        out.compared = {0, 4, 7};  // across x = 0, over the north pole, across longitude 180
        for (const std::size_t i : out.compared) {
            WalkConfig w = out.walks[i];
            w.legs = 2;
            w.merged = true;
            out.merged.push_back(run_walk(w));
            w.merged = false;
            w.ring = Ring::adjacency;
            out.adjacency.push_back(run_walk(w));
        }
        return out;
    }();
    return runs;
}

// Regenerate only on purpose: a different value means the field, the selection, the mesher, the brush arithmetic,
// Jolt or its build flags changed. The INFO lines of a failing run print the new values.
constexpr std::uint64_t lod_expected = 0x97715b03a237437dULL;
constexpr std::uint64_t dig_expected = 0x3cfd9dbf51875426ULL;
constexpr std::uint64_t walk_expected = 0xcdb2b78d799d3a05ULL;
constexpr std::uint64_t edges_expected = 0xa3aba7a8cbc3268dULL;
constexpr std::uint64_t pole_landing_expected = 0xcd52b59f83d39b34ULL;

}  // namespace

TEST_CASE(
    "[seams.AC1] the multi-level octree welds watertight at the octant planes, the poles and the cube-corner "
    "directions, and the field is continuous across the planes") {
    {
        std::vector<std::uint64_t> hashes;
        for (const LodRun& r : lod_runs()) {
            INFO(std::format(
                "{}: {} leaves, {} meshed (L0 {} L1 {} L2 {}), {} triangles, {} open, {} non-manifold, "
                "{} shared plane points, {} mismatched; mesh {}",
                r.spec.name, r.lod.leaves, r.lod.meshed, r.lod.meshed_per_level[0], r.lod.meshed_per_level[1],
                r.lod.meshed_per_level[2], r.lod.seams.triangles, r.lod.seams.open_edges, r.lod.seams.nonmanifold_edges,
                r.lod.shared_points, r.lod.shared_mismatches, hex(r.lod.mesh_hash)));
            CHECK(r.lod.seams.open_edges == 0);
            CHECK(r.lod.seams.nonmanifold_edges == 0);
            // LOD0 around the site and the transition ring to level 1 hold surface (level 2 too, except at the
            // corner sites, where every root is split).
            CHECK(r.lod.meshed_per_level[0] > 0);
            CHECK(r.lod.meshed_per_level[1] > 0);
            CHECK(r.lod.transition_triangles > 0);
            CHECK(r.lod.shared_mismatches == 0);
            if (octant_planes(r.site) != 0) {
                CHECK(r.lod.shared_points > 1000);  // the check saw both sides of the plane
            }
            for (const auto& [axis, c] : r.continuity) {
                INFO(std::format("plane {} = 0: step across {} m, within {} m, jump {} m", "xyz"[axis],
                                 c.max_step_across, c.max_step_within, c.max_jump));
                CHECK(c.max_step_across <= c.lipschitz_bound);
                CHECK(c.max_step_across <= c.max_step_within + 0.05);  // no step at the plane
                CHECK(c.max_jump < 1e-9);
            }
            hashes.push_back(r.lod.mesh_hash);
        }
        const std::uint64_t h = voxel::combine_hashes(hashes);
        INFO("lod " << hex(h));
        CHECK(h == lod_expected);
    }
}

TEST_CASE("[seams.AC2] SP-12's dig and collide works at every site: watertight, and the ball falls in and rests") {
    {
        std::vector<std::uint64_t> hashes;
        for (std::size_t i = 0; i < sweep_sites().size(); ++i) {
            const SiteSpec& spec = sweep_sites()[i];
            const sp12::ScenarioResult& d = dig_runs()[i];
            INFO(
                std::format("{}: {} sampled, {} changed, {} open edges; before the pit {} m, before the sump {} m, end "
                            "{} m deep, {} m off, {} m/s; samples {} mesh {} colliders {} chain {}",
                            spec.name, d.sampled, d.changed, d.open_edges, d.batches[0].depth_before,
                            d.batches[1].depth_before, d.depth, d.lateral, d.speed, hex(d.samples_hash),
                            hex(d.mesh_hash), hex(d.colliders_hash), hex(d.chain)));
            CHECK(d.outside == 0);
            CHECK(d.mismatches == 0);
            CHECK(d.open_edges == 0);
            CHECK(d.terrain_bodies == d.meshed);
            CHECK(d.batches[0].depth_before > -0.2);  // on the ground before the dig
            CHECK(d.batches[0].depth_before < 0.6);
            CHECK(d.batches[1].depth_before < -2.0);  // in the pit before the sump
            CHECK(d.depth < -3.5);                    // in the sump at the end, at rest
            CHECK(d.lateral < 1.5);
            CHECK(d.speed < 0.01);
            for (const std::uint64_t v :
                 {d.base_samples_hash, d.samples_hash, d.mesh_hash, d.colliders_hash, d.install_hash, d.chain}) {
                hashes.push_back(v);
            }
        }
        const std::uint64_t h = voxel::combine_hashes(hashes);
        INFO("dig " << hex(h));
        CHECK(h == dig_expected);
    }
}

TEST_CASE(
    "[seams.AC3] [seams.AC4] a character walks across octant planes, an 8-chunk corner and over both poles, chunk "
    "collider borders") {
    SUBCASE(
        "[seams.AC3] a character walks across octant planes, an 8-chunk corner and over both poles on the "
        "ground, with radial up and a continuous heading") {
        const Walks& s = walk_runs();
        std::vector<std::uint64_t> hashes;
        for (std::size_t i = 0; i < s.walks.size(); ++i) {
            const WalkResult& r = s.walked[i];
            INFO(std::format(
                "{}: {} steps, {} unsupported, feet {}..{} m, |dv| boundary max {} interior max {}, up "
                "error {}, heading step {}, compass jump {}, {} m from the midpoint; chain {}",
                s.walks[i].name, r.steps, r.unsupported, r.min_gap, r.max_gap, r.max_dv_boundary, r.max_dv_interior,
                r.max_up_error, r.max_heading_step, r.compass_jump, r.max_from_mid, hex(r.chain)));
            CHECK(r.steps > 1000);
            CHECK(r.unsupported == 0);  // never off the ground
            CHECK(r.min_gap > -0.01);   // never into it
            CHECK(r.max_from_mid < s.walks[i].half_length + 1.0);
            CHECK(r.chunk_crossings >= 4);
            CHECK(r.max_up_error < 1e-6);      // up stays radial
            CHECK(r.max_heading_step < 1e-6);  // the heading is parallel-transported: no jump, at the poles too
            CHECK(r.max_dv_boundary < 2.5);    // the border-edge hitches of [seams.AC4] are the largest
            hashes.push_back(r.chain);
        }
        CHECK(s.walked[4].octant_crossings >= 4);  // the north-pole walk crosses x = 0 and z = 0 each way
        CHECK(s.walked[5].compass_jump > 2.5);     // walking over the pole, a compass heading flips by about π
        const std::uint64_t h = voxel::combine_hashes(hashes);
        INFO("walk " << hex(h));
        CHECK(h == walk_expected);
    }
    SUBCASE(
        "[seams.AC4] chunk colliders bump a walking character at their border edges; neighbor triangles as "
        "adjacency remove it") {
        const Walks& s = walk_runs();
        std::vector<std::uint64_t> hashes;
        for (std::size_t k = 0; k < s.compared.size(); ++k) {
            const WalkResult& chunks = s.walked[s.compared[k]];
            const WalkResult& merged = s.merged[k];
            const WalkResult& adjacency = s.adjacency[k];
            INFO(std::format("{}: p99 |dv| near chunk borders: chunks {}, merged {}, adjacency {} m/s",
                             s.walks[s.compared[k]].name, chunks.p99_dv_boundary, merged.p99_dv_boundary,
                             adjacency.p99_dv_boundary));
            CHECK(merged.colliders == 1);
            CHECK(chunks.p99_dv_boundary > merged.p99_dv_boundary + 0.15);
            CHECK(std::abs(adjacency.p99_dv_boundary - merged.p99_dv_boundary) < 0.05);
            CHECK(adjacency.unsupported == 0);
            hashes.push_back(merged.chain);
            hashes.push_back(adjacency.chain);
        }
        const std::uint64_t h = voxel::combine_hashes(hashes);
        INFO("edges " << hex(h));
        CHECK(h == edges_expected);
    }
}

TEST_CASE("[seams.AC5] the cube-mapped procgen inputs have no seam at the face edges") {
    using namespace veach::seams::cubemap;
    constexpr int n = static_cast<int>(sp07::face_size);
    const std::vector<Pair> pairs = neighbor_pairs(n);
    const sp07::Body body = sp07::pick_body();
    const sp07::Inputs in = sp07::make_inputs();
    sp07::Outputs out;
    procgen::Sandbox sandbox;
    auto hook = sandbox.load(sp07::read_hook(), "sp07_climate.luau");
    REQUIRE(hook.has_value());
    REQUIRE(sp07::run(sandbox, *hook, body, in, out).has_value());
    auto program = sp06::terrain_program();
    REQUIRE(program.has_value());
    const std::vector<float> field = field_graph_map(*program, n);
    const struct {
        const char* name;
        Gradient g;
    } inputs[] = {
        {"altitude", gradient(std::span<const double>(in.altitude), pairs)},
        {"temperature", gradient(std::span<const double>(out.temperature), pairs)},
        {"precipitation", gradient(std::span<const double>(out.precipitation), pairs)},
        {"SP-06 field graph", gradient(std::span<const float>(field), pairs)},
    };
    for (const auto& [name, g] : inputs) {
        // Across an edge, values change no faster than between neighbors on one face next to it.
        INFO(std::format("{}: |Δ|/angle p99 within {} across {}, max within {} across {}", name, g.within_p99,
                         g.across_p99, g.within_max, g.across_max));
        CHECK(g.across == 768);
        CHECK(g.across_p99 < 1.5 * g.within_p99);
        CHECK(g.across_max < 1.5 * g.within_max);
    }
    const Gradient biome = gradient(std::span<const std::uint8_t>(out.biome), pairs);
    INFO(std::format("biome changes: within {}, across {}", biome.within_changed, biome.across_changed));
    CHECK(biome.across_changed < 1.5 * biome.within_changed);
}

TEST_CASE("[seams.AC6] SP-13's craft lands on a pad at the north pole and a character walks around it") {
    sp13::Sim sim(sp13::Config{.scale = 0.01, .latitude = 90.0});
    sp13::Autopilot pilot(sim.planet(), false);
    sp13::Controls c;
    bool supported = true;
    for (int i = 0; i < 60 * 600 && pilot.phase() != sp13::Autopilot::Phase::done; ++i) {
        sim.step(c);
        c = pilot.next(sim.craft());
        if (sim.tick() > 300) {
            supported = supported && sim.walker_supported();
        }
    }
    INFO(std::format("hover {}..{} m, touchdown {} m/s, {} m from the pad; hash {}", pilot.hover_min(),
                     pilot.hover_max(), pilot.touchdown_speed(), pilot.landing_offset(), hex(sim.hash())));
    REQUIRE(pilot.phase() == sp13::Autopilot::Phase::done);
    CHECK(pilot.hover_min() > 48.0);
    CHECK(pilot.hover_max() < 52.0);
    CHECK(pilot.touchdown_speed() < 1.0);
    CHECK(pilot.landing_offset() < 1.0);
    CHECK(supported);
    CHECK(sim.hash() == pole_landing_expected);
}
