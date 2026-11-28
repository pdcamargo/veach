// Terrain streaming in engine/voxel (docs/specs/voxel-terrain/spec.md, VCH-114): density from field programs, the
// LOD selection, streaming with a budget, edits and colliders. The committed hashes were made on macOS arm64; Windows
// x64 CI checks them (sampling, edits and meshing are canonical, ADR 0043).

#include <doctest/doctest.h>

#include "sp12/sp12_scenario.hpp"
#include "sp12/sp12_terrain.hpp"
#include "support/terrain_fields.hpp"
#include "support/test_support.hpp"

#include <veach/core/jobs.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>
#include <veach/runtime/engine.hpp>
#include <veach/runtime/terrain.hpp>
#include <veach/voxel/terrain.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace {

using namespace veach;
using namespace veach::voxel;

std::string hex(std::uint64_t v) {
    return std::format("0x{:016x}", v);
}

LatticeBox box_of(const ChunkKey& k) {
    const auto o = chunk_origin(k);
    const std::int64_t e = chunk_units(k.level);
    return {o, {o[0] + e, o[1] + e, o[2] + e}};
}

/// The flat world's octree: 256 m roots, 1 km x 512 m x 1 km.
OctreeBounds flat_bounds() {
    return {.root_level = 4, .root_min = {-2, -1, -2}, .root_max = {1, 0, 1}};
}

TerrainSettings flat_settings(std::uint32_t budget = 1000) {
    TerrainSettings s;
    s.octree = flat_bounds();
    s.lambda = 2.0;
    s.budget.jobs_per_frame = budget;
    s.asynchronous = false;
    return s;
}

/// Every pair of touching leaves differs by at most one level.
std::size_t restriction_violations(const std::vector<ChunkSlot>& leaves) {
    std::size_t bad = 0;
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const LatticeBox a = box_of(leaves[i].key);
        for (std::size_t j = i + 1; j < leaves.size(); ++j) {
            const int d = std::abs(int{leaves[i].key.level} - int{leaves[j].key.level});
            if (d > 1 && a.intersects(box_of(leaves[j].key))) {
                ++bad;
            }
        }
    }
    return bad;
}

/// Runs updates until the drawn set is the selection and nothing is in flight (at most `limit` updates).
int settle(TerrainBody& body, JobSystem& jobs, int limit = 200) {
    for (int i = 0; i < limit; ++i) {
        const StreamStats s = body.update(jobs);
        if (s.stale == 0 && s.launched == 0 && s.completed == 0) {
            return i;
        }
    }
    return limit;
}

}  // namespace

TEST_CASE("[voxel.AC2] program samples are a function of the lattice point, and bounds never hide a surface") {
    const auto hills = test::flat_hills();
    // A level-0 chunk at the surface and the level-1 chunk holding it: equal samples at every shared point.
    const ChunkKey fine{0, 0, -1, 0};
    const ChunkKey coarse{1, 0, -1, 0};
    ChunkSamples a, b, east;
    hills->sample(fine, 0, a);
    hills->sample(coarse, 1u << Face::pos_x, b);  // and its full-resolution +x face
    hills->sample({0, 2, -1, 0}, 0, east);        // the level-0 chunk across that face
    const auto oa = chunk_origin(fine), ob = chunk_origin(coarse), oe = chunk_origin({0, 2, -1, 0});
    int shared = 0, differ = 0;
    for (int z = -1; z <= chunk_cells + 1; ++z) {
        for (int y = -1; y <= chunk_cells + 1; ++y) {
            for (int x = -1; x <= chunk_cells + 1; ++x) {
                const std::array<std::int64_t, 3> p{oa[0] + x, oa[1] + y, oa[2] + z};
                std::array<std::int64_t, 3> q{};
                bool on = true;
                for (std::size_t i = 0; i < 3; ++i) {
                    q[i] = p[i] - ob[i];
                    on = on && q[i] % 2 == 0 && q[i] >= -2 && q[i] <= 2 * (chunk_cells + 1);
                }
                if (!on) {
                    continue;
                }
                const Sample& s0 = a.at(x, y, z);
                const Sample& s1 =
                    b.at(static_cast<int>(q[0] / 2), static_cast<int>(q[1] / 2), static_cast<int>(q[2] / 2));
                differ += s0.distance != s1.distance || s0.material != s1.material;
                ++shared;
            }
        }
    }
    CHECK(shared == 17 * 17 * 17);
    CHECK(differ == 0);
    // The coarse chunk's +x face (half spacing = LOD0) against the fine chunk's grid across the plane.
    int face_shared = 0, face_differ = 0;
    for (int v = -1; v <= 2 * chunk_cells + 1; ++v) {
        for (int u = -1; u <= 2 * chunk_cells + 1; ++u) {
            // u along y, v along z; layer 0 is the plane x = ob + 64 = oe.
            const Sample& f = b.face_at(Face::pos_x, u, v, 0);
            const std::int64_t py = ob[1] + u, pz = ob[2] + v;
            const int ey = static_cast<int>(py - oe[1]), ez = static_cast<int>(pz - oe[2]);
            if (ey < -1 || ez < -1 || ey > chunk_cells + 1 || ez > chunk_cells + 1) {
                continue;
            }
            face_differ += f.distance != east.at(0, ey, ez).distance;
            ++face_shared;
        }
    }
    CHECK(face_shared > 1000);
    CHECK(face_differ == 0);
    CHECK(a.has_surface);
    INFO(std::format("fine {} coarse {}", hex(samples_hash(a)), hex(samples_hash(b))));
    CHECK(samples_hash(a) == 0x7ef50311aa57d763ULL);
    CHECK(samples_hash(b) == 0x669c899a3caa74cfULL);

    // Bounds: every chunk classified empty or solid really is, and the bounds decide most chunks away from the
    // surface.
    for (const auto& density : {test::flat_hills(), test::lumpy_sphere()}) {
        std::size_t wrong = 0, decided = 0, total = 0;
        const bool sphere = density->settings().shape == BaseShape::sphere;
        for (int level = 0; level <= 2; ++level) {
            for (int z = -3; z <= 2; ++z) {
                for (int y = -3; y <= 2; ++y) {
                    const std::int32_t base = sphere ? static_cast<std::int32_t>(800 / chunk_units(level)) : 0;
                    for (int x = -1; x <= 1; ++x) {
                        const ChunkKey k{static_cast<std::uint8_t>(level), base + x, sphere ? z : y, sphere ? y : z};
                        const BoxClass c = density->classify(sample_box(k));
                        ++total;
                        if (c == BoxClass::surface) {
                            continue;
                        }
                        ++decided;
                        ChunkSamples s;
                        density->sample(k, 0x3f, s);
                        bool neg = false, pos = false;
                        for (const Sample& v : s.grid) {
                            (v.distance < 0 ? neg : pos) = true;
                        }
                        for (int f = 0; f < 6 && k.level > 0; ++f) {
                            for (const Sample& v : s.faces[static_cast<std::size_t>(f)]) {
                                (v.distance < 0 ? neg : pos) = true;
                            }
                        }
                        wrong += (c == BoxClass::empty && neg) || (c == BoxClass::solid && pos);
                    }
                }
            }
        }
        CHECK(wrong == 0);
        CHECK(decided > total / 3);
    }

    // Settings that can't work name the fix.
    auto missing = ProgramDensity::make(test::flat_hills()->program(), {.density_output = "nope"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::voxel_density_invalid);
    CHECK(missing.error().hint.find("height") != std::string::npos);
    auto radius = ProgramDensity::make(test::flat_hills()->program(), {.radius_m = 0.0, .density_output = "height"});
    REQUIRE_FALSE(radius.has_value());
    CHECK(radius.error().code == ErrorCode::voxel_density_invalid);
}

TEST_CASE("[voxel.AC3] the selection is restricted, follows every viewer, prunes empty space, and has hysteresis") {
    const auto hills = test::flat_hills();
    JobSystem jobs(4);
    TerrainBody body(hills, flat_settings());
    const auto classify = [&](std::span<const ChunkKey> keys, std::span<BoxClass> out) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            out[i] = hills->classify(sample_box(keys[i]));
        }
    };
    LodSelector selector(flat_bounds());
    const std::array<std::array<double, 3>, 2> viewers{{{10.0, 8.0, 10.0}, {-300.0, 8.0, 250.0}}};
    const std::vector<ChunkSlot> leaves = selector.select(viewers, 2.0, 0.1, classify);
    REQUIRE(!leaves.empty());
    CHECK(restriction_violations(leaves) == 0);
    CHECK(std::ranges::is_sorted(leaves));
    // Level 0 under both viewers, nothing pruned, and only boxes that may hold the surface.
    for (const auto& v : viewers) {
        const ChunkKey under{0, static_cast<std::int32_t>(std::floor(v[0] / 16.0)), -1,
                             static_cast<std::int32_t>(std::floor(v[2] / 16.0))};
        const bool found = std::ranges::any_of(
            leaves, [&](const ChunkSlot& s) { return s.key.level == 0 && s.key.x == under.x && s.key.z == under.z; });
        CHECK(found);
    }
    for (const ChunkSlot& s : leaves) {
        CHECK(hills->classify(sample_box(s.key)) == BoxClass::surface);
    }
    // Finer faces point at a neighbor that was split.
    std::set<ChunkKey> keys;
    for (const ChunkSlot& s : leaves) {
        keys.insert(s.key);
    }
    std::size_t finer_faces = 0, finer_wrong = 0;
    for (const ChunkSlot& s : leaves) {
        for (int f = 0; f < 6; ++f) {
            if ((s.finer & (1u << f)) == 0) {
                continue;
            }
            ++finer_faces;
            ChunkKey n = s.key;
            (f / 2 == 0 ? n.x : (f / 2 == 1 ? n.y : n.z)) += (f & 1) ? 1 : -1;
            finer_wrong += keys.contains(n) ? 1 : 0;  // a same-level leaf there is not finer
        }
    }
    CHECK(finer_faces > 0);
    CHECK(finer_wrong == 0);

    // Hysteresis: find two viewer positions 0.5 m apart that select differently without it; with 0.1, hovering
    // between them changes the selection at most once.
    std::array<double, 3> p0{0.0, 8.0, 0.0}, p1{};
    bool found = false;
    {
        LodSelector probe(flat_bounds());
        std::vector<ChunkSlot> last = probe.select(std::span(&p0, 1), 2.0, 0.0, classify);
        for (double x = 0.5; x < 200.0 && !found; x += 0.5) {
            p1 = {x, 8.0, 0.0};
            LodSelector fresh(flat_bounds());
            const auto now = fresh.select(std::span(&p1, 1), 2.0, 0.0, classify);
            if (now != last) {
                p0 = {x - 0.5, 8.0, 0.0};
                found = true;
            }
            last = now;
        }
    }
    REQUIRE(found);
    for (const double h : {0.0, 0.1}) {
        LodSelector hover(flat_bounds());
        std::vector<ChunkSlot> last;
        int changes = 0;
        for (int i = 0; i < 20; ++i) {
            const auto& p = i % 2 == 0 ? p0 : p1;
            const auto now = hover.select(std::span(&p, 1), 2.0, h, classify);
            changes += i > 0 && now != last ? 1 : 0;
            last = now;
        }
        if (h == 0.0) {
            CHECK(changes == 19);
        } else {
            CHECK(changes <= 1);
        }
    }
}

TEST_CASE("[voxel.AC4] a descent never draws a hole, and a backed-up stream lowers lambda instead of stalling") {
    const auto rock = test::lumpy_sphere(200.0);
    JobSystem jobs(4);
    TerrainSettings s;
    s.octree = OctreeBounds::around_origin(rock->max_extent() + 16.0);
    s.lambda = 2.5;
    s.budget = {.jobs_per_frame = 6, .min_lambda = 1.5, .patience = 4};
    s.asynchronous = true;
    TerrainBody body(rock, s);
    const std::array<double, 3> up{0.36, 0.48, 0.8};
    double lowest_lambda = s.lambda;
    std::size_t checked = 0, open = 0, empty_after_first = 0;
    std::uint32_t max_stale = 0;
    bool drew = false;
    std::uint64_t last_drawn = 0;
    const auto check = [&](const StreamStats& st) {
        lowest_lambda = std::min(lowest_lambda, body.lambda());
        max_stale = std::max(max_stale, st.stale);
        if (body.drawn().empty()) {
            empty_after_first += drew ? 1 : 0;
            return;
        }
        drew = true;
        if (body.drawn_hash() == last_drawn) {
            return;  // the weld check only when the drawn set changed
        }
        last_drawn = body.drawn_hash();
        const WeldReport w = body.weld();
        open += w.open;
        ++checked;
    };
    // From 800 m above the surface down to 10 m in 100 frames (exponential, like the travel governor), then hover.
    for (int frame = 0; frame < 160; ++frame) {
        const double t = std::min(1.0, frame / 100.0);
        const double altitude = 800.0 * std::pow(10.0 / 800.0, t);
        const double r = rock->settings().radius_m + 20.0 + altitude;
        const std::array<double, 3> viewer{up[0] * r, up[1] * r, up[2] * r};
        body.set_viewers(std::span(&viewer, 1));
        check(body.update(jobs));
    }
    CHECK(checked > 8);  // distinct drawn sets
    CHECK(open == 0);
    CHECK(empty_after_first == 0);
    CHECK(lowest_lambda < s.lambda);  // the budget was too small: it went coarser...
    CHECK(lowest_lambda >= s.budget.min_lambda);
    // ...and recovers once the stream keeps up.
    for (int frame = 0; frame < 600 && (body.lambda() < s.lambda || body.update(jobs).stale != 0); ++frame) {
        check(body.update(jobs));
    }
    CHECK(body.lambda() == doctest::Approx(s.lambda));
    CHECK(settle(body, jobs) < 200);
    CHECK(body.weld().open == 0);
    CHECK(open == 0);
    // Level 0 is drawn under the viewer at 10 m.
    CHECK(std::ranges::any_of(body.drawn(), [](const ChunkRef& r) { return r->slot.key.level == 0; }));
    INFO(std::format("max stale {}, lowest lambda {}", max_stale, lowest_lambda));
}

TEST_CASE("[voxel.AC5] chunk hashes are the same with 1 and 4 workers, synchronous or across frames") {
    const auto hills = test::flat_hills();
    struct Run {
        std::vector<std::uint64_t> per_update;
        std::uint64_t settled = 0;
        std::size_t drawn = 0;
    };
    const auto run = [&](std::uint32_t workers, bool asynchronous) {
        JobSystem jobs(workers);
        TerrainSettings s = flat_settings(12);
        s.asynchronous = asynchronous;
        TerrainBody body(hills, s);
        Run r;
        for (int i = 0; i < 24; ++i) {
            const std::array<double, 3> viewer{-40.0 + 4.0 * i, 6.0, 20.0 - 2.0 * i};
            body.set_viewers(std::span(&viewer, 1));
            body.update(jobs);
            r.per_update.push_back(body.drawn_hash());
        }
        settle(body, jobs);
        r.settled = body.drawn_hash();
        r.drawn = body.drawn().size();
        return r;
    };
    const Run one = run(1, false);
    const Run four = run(4, false);
    const Run async = run(4, true);
    CHECK(one.per_update == four.per_update);
    CHECK(one.settled == four.settled);
    CHECK(async.settled == one.settled);
    CHECK(one.drawn > 20);
    INFO(std::format("settled {} ({} chunks)", hex(one.settled), one.drawn));
    CHECK(one.settled == 0xedbfb8fcac48a406ULL);
}

TEST_CASE("[voxel.AC6] a chunk is remeshed with transition cells when a neighbor's level changes") {
    const auto hills = test::flat_hills();
    JobSystem jobs(4);
    TerrainBody body(hills, flat_settings());
    std::array<double, 3> viewer{0.0, 6.0, 0.0};
    body.set_viewers(std::span(&viewer, 1));
    settle(body, jobs);
    std::map<ChunkKey, std::uint8_t> before;
    for (const ChunkRef& r : body.drawn()) {
        before[r->slot.key] = r->slot.finer;
    }
    viewer = {60.0, 6.0, 0.0};
    body.set_viewers(std::span(&viewer, 1));
    const StreamStats first = body.update(jobs);
    CHECK(first.remeshes > 0);
    settle(body, jobs);
    std::size_t remeshed = 0, with_transitions = 0;
    for (const ChunkRef& r : body.drawn()) {
        const auto it = before.find(r->slot.key);
        if (it != before.end() && it->second != r->slot.finer) {
            ++remeshed;
            if (r->slot.finer != 0 && r->mesh.transition_triangles > 0) {
                ++with_transitions;
            }
        }
    }
    CHECK(remeshed > 0);
    CHECK(with_transitions > 0);
    CHECK(body.weld().open == 0);
    // The cracked control: plain marching cubes across the same levels leaves open edges.
    TerrainSettings plain = flat_settings();
    plain.mesher.transitions = false;
    TerrainBody cracked(hills, plain);
    cracked.set_viewers(std::span(&viewer, 1));
    settle(cracked, jobs);
    CHECK(cracked.weld().open > 0);
}

TEST_CASE("[voxel.AC7] the edit store replays SP-12's dig to its committed hashes, with owned override records") {
    const sp12::PlanetField field;
    const FieldSource source(field, DistanceEncoding::linear);
    const std::vector<BrushOp> log = sp12::dig_log();
    const ChunkKey center = sp12::chunk_of_mm(sp12::dig_site_mm);
    // SP-12's chunk set: the region's chunks that may hold a surface, plus those an op touched (sampled on edit).
    std::set<ChunkKey> set;
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const ChunkKey k{0, center.x + dx, center.y + dy, center.z + dz};
                if (field.may_have_surface(k)) {
                    set.insert(k);
                }
            }
        }
    }
    std::vector<ChunkKey> surface_keys;  // what SP-12 sampled before the dig
    for (const ChunkKey& k : set) {
        if (field.may_have_surface(k)) {
            surface_keys.push_back(k);
        }
    }
    for (const BrushOp& op : log) {
        for (const ChunkKey& k : touched_chunks(op)) {
            if (std::abs(k.x - center.x) <= 2 && std::abs(k.y - center.y) <= 2 && std::abs(k.z - center.z) <= 2) {
                set.insert(k);
            }
        }
    }
    const std::vector<ChunkKey> keys(set.begin(), set.end());
    const auto hashes = [&](const TerrainBody& body, JobSystem& jobs, const std::vector<ChunkKey>& keys) {
        std::vector<std::uint64_t> samples(keys.size()), meshes(keys.size());
        jobs.parallel_for("test", keys.size(), 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                samples[i] = samples_hash(body.samples(keys[i]));
                meshes[i] = body.build({keys[i], 0}).mesh_hash;
            }
        });
        return std::pair{combine_hashes(samples), combine_hashes(meshes)};
    };
    TerrainSettings settings;
    settings.octree = OctreeBounds::around_origin(sp12::PlanetField::radius + 100.0);
    settings.asynchronous = false;
    std::uint64_t store_hash = 0;
    for (const std::uint32_t workers : {1u, 4u}) {
        JobSystem jobs(workers);
        TerrainBody body(std::shared_ptr<const DensitySource>(&source, [](const DensitySource*) {}), settings);
        const auto base = hashes(body, jobs, surface_keys);
        INFO(std::format("base samples {} mesh {}", hex(base.first), hex(base.second)));
        CHECK(base.first == 0xace58c96ce261f0dULL);   // SP-12's base_samples_expected
        CHECK(base.second == 0x796b6be1210887a9ULL);  // base_mesh_expected
        std::size_t materialized = 0;
        for (const BrushOp& op : log) {
            auto r = body.apply(op, jobs);
            REQUIRE(r.has_value());
            CHECK(r->seq == op.seq);
            materialized += r->materialized;
        }
        const auto edited = hashes(body, jobs, keys);
        INFO(std::format("edited samples {} mesh {}", hex(edited.first), hex(edited.second)));
        CHECK(edited.first == 0xd0163ab3d7069434ULL);   // SP-12's samples_expected
        CHECK(edited.second == 0x1be19fd52b71cdadULL);  // mesh_expected
        CHECK(materialized > 0);
        // Records hold owned samples only: each lattice point once.
        for (const auto& [key, record] : body.edits().records()) {
            CHECK(key.level == 0);
            CHECK(record.overridden > 0);
            CHECK(record.overridden <= OverrideRecord::owned_count);
        }
        CHECK(body.edits().journal().size() == log.size());
        if (workers == 1) {
            store_hash = body.edits().hash();
        } else {
            CHECK(body.edits().hash() == store_hash);
        }

        // Coarse levels read the overrides at their own lattice points.
        const ChunkKey parent{1, center.x >> 1, center.y >> 1, center.z >> 1};
        const ChunkSamples coarse = body.samples(parent);
        const auto op = chunk_origin(parent);
        int compared = 0, differ = 0;
        for (const ChunkKey& k : keys) {
            if ((k.x >> 1) != parent.x || (k.y >> 1) != parent.y || (k.z >> 1) != parent.z) {
                continue;
            }
            const ChunkSamples fine = body.samples(k);
            const auto ok = chunk_origin(k);
            for (int z = 0; z < chunk_cells; z += 2) {
                for (int y = 0; y < chunk_cells; y += 2) {
                    for (int x = 0; x < chunk_cells; x += 2) {
                        const auto cx = static_cast<int>((ok[0] + x - op[0]) / 2);
                        const auto cy = static_cast<int>((ok[1] + y - op[1]) / 2);
                        const auto cz = static_cast<int>((ok[2] + z - op[2]) / 2);
                        differ += fine.at(x, y, z).distance != coarse.at(cx, cy, cz).distance;
                        ++compared;
                    }
                }
            }
        }
        CHECK(compared > 0);
        CHECK(differ == 0);

        // Ops are ordered and checked.
        BrushOp late = log.front();
        late.seq = 3;
        auto refused = body.apply(late, jobs);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::voxel_brush_invalid);
        BrushOp huge = log.front();
        huge.seq = 0;
        huge.radius_mm = max_brush_radius_mm + 1;
        CHECK_FALSE(body.apply(huge, jobs).has_value());
    }
    INFO(std::format("edit store {}", hex(store_hash)));
    CHECK(store_hash == 0xf3323260a5bf8e25ULL);

    // Saved records restore onto a fresh body to the same samples; a wrong base hash is reported, and applied anyway.
    JobSystem jobs(4);
    TerrainBody edited(std::shared_ptr<const DensitySource>(&source, [](const DensitySource*) {}), settings);
    for (const BrushOp& op : log) {
        REQUIRE(edited.apply(op, jobs).has_value());
    }
    TerrainBody loaded(std::shared_ptr<const DensitySource>(&source, [](const DensitySource*) {}), settings);
    test::LogCapture logs;
    bool all_matched = true;
    for (const auto& [key, record] : edited.edits().records()) {
        all_matched = loaded.restore(record) && all_matched;
    }
    CHECK(all_matched);
    CHECK(loaded.edits().hash() == edited.edits().hash());
    OverrideRecord bad = edited.edits().records().begin()->second;
    bad.base_hash ^= 1;
    CHECK_FALSE(loaded.restore(bad));
    CHECK(logs.contains("regenerated base"));
}

namespace {

struct CaptureSink final : ColliderSink {
    std::vector<ColliderUpdate> updates;
    void submit(ColliderUpdate update) override { updates.push_back(std::move(update)); }
};

}  // namespace

TEST_CASE("[voxel.AC8] colliders: LOD0 meshes with an adjacency ring, for install at t + 2, recooked on edits") {
    const auto hills = test::flat_hills();
    JobSystem jobs(4);
    TerrainBody body(hills, flat_settings());
    CaptureSink sink;
    body.set_collider_sink(&sink);
    const ColliderProbe probe{.position_m = {8.0, 3.0, 8.0}, .velocity_m_s = {40.0, 0.0, 0.0}};
    const ColliderStats first = body.update_colliders(std::span(&probe, 1), 100, jobs);
    REQUIRE(sink.updates.size() == 1);
    const ColliderUpdate u = sink.updates[0];  // a copy: the sink keeps growing
    CHECK(u.tick == 100);
    CHECK(u.install_tick == 102);
    CHECK(first.installed == u.install.size());
    CHECK(u.install.size() >= 9);  // reach 32 m along a 10 m path, on the surface layer
    CHECK(std::ranges::is_sorted(u.install, {}, &ColliderMesh::key));
    std::size_t with_ring = 0;
    std::vector<std::uint64_t> hashes;
    for (const ColliderMesh& c : u.install) {
        CHECK(c.key.level == 0);
        const ChunkRecord own = body.build({c.key, 0});
        CHECK(c.own_triangles == own.mesh.triangle_count());
        with_ring += c.indices.size() / 3 > c.own_triangles ? 1 : 0;
        // Welded: every position is distinct.
        std::set<std::array<std::int64_t, 3>> unique(c.positions.begin(), c.positions.end());
        CHECK(unique.size() == c.positions.size());
        hashes.push_back(c.hash);
    }
    CHECK(with_ring == u.install.size());
    const std::uint64_t colliders = combine_hashes(hashes);
    INFO(std::format("colliders {}", hex(colliders)));
    // A ring's hash covers its chunk and the neighbors with triangles (ADR 0088: since collider chunks classify
    // shallow, neighbors without triangles no longer enter it; the meshes and shapes are the same as before).
    CHECK(colliders == 0xbede9c57daef8a37ULL);

    // Nothing changed: nothing to submit.
    body.update_colliders(std::span(&probe, 1), 101, jobs);
    CHECK(sink.updates.size() == 1);

    // A dig on a chunk border: the chunks it changed and the ones whose ring they feed are recooked.
    // On the surface at (16, y, 8) m, a chunk border plane.
    double lo = -20.0, hi = 20.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        (hills->distance(16.0, mid, 8.0) < 0.0 ? lo : hi) = mid;
    }
    BrushOp dig;
    dig.kind = BrushKind::subtract;
    dig.center_mm = {16'000, static_cast<std::int64_t>(std::floor(lo * 1000.0)), 8'000};
    dig.radius_mm = 2'000;
    const auto edit = body.apply(dig, jobs);
    REQUIRE(edit.has_value());
    CHECK(edit->changed.size() >= 2);
    body.update_colliders(std::span(&probe, 1), 110, jobs);
    REQUIRE(sink.updates.size() == 2);
    const ColliderUpdate recook = sink.updates[1];
    CHECK(recook.install_tick == 112);
    CHECK(recook.install.size() > edit->changed.size());  // ring-fed neighbors too
    for (const ChunkKey& k : edit->changed) {
        const bool wanted = std::ranges::any_of(u.install, [&](const ColliderMesh& c) { return c.key == k; });
        if (wanted) {
            CHECK(std::ranges::any_of(recook.install, [&](const ColliderMesh& c) { return c.key == k; }));
        }
    }

    // Moving away removes them.
    const ColliderProbe far{.position_m = {400.0, 3.0, -400.0}};
    body.update_colliders(std::span(&far, 1), 120, jobs);
    REQUIRE(sink.updates.size() == 3);
    CHECK(sink.updates[2].remove.size() == u.install.size());
}

TEST_CASE("[voxel.AC9] a body in a reference frame streams around a TerrainViewer into the MeshStore and draws") {
    test::TempProject project("voxel-terrain");
    Engine engine(test::headless_config(project.path(), {}, 1000));
    engine.import<TerrainModule>();
    REQUIRE(engine.init().has_value());
    World& world = engine.world();
    auto& bodies = services(world).get<TerrainBodies>();
    auto& meshes = services(world).get<render::MeshStore>();
    const AssetId grass = meshes.add_material({.name = "grass"});
    // The flat world 10 km out and turned a quarter about +Y: positions go through the frame in double.
    const Entity ground = world.entity("ground").set(
        ReferenceFrame{.position = math::dvec3(10'000.0, 0.0, 0.0),
                       .orientation = math::angleAxis(1.5707963267948966, math::dvec3(0.0, 1.0, 0.0))});
    TerrainSettings s = flat_settings(64);
    s.asynchronous = true;
    bodies.add(ground, std::make_shared<TerrainBody>(test::flat_hills(), s), {.materials = {grass}});
    const Entity camera = world.entity("camera")
                              .child_of(ground)
                              .set(Transform{.translation = math::dvec3(10.0, 6.0, 10.0)})
                              .set(Camera{.fov_y = math::radians(60.0f), .near_plane = 0.1f, .active = true})
                              .add<TerrainViewer>();
    REQUIRE(engine.start().has_value());
    for (int i = 0; i < 40; ++i) {
        engine.frame();
    }
    const StreamStats* stats = bodies.stats(ground);
    REQUIRE(stats != nullptr);
    CHECK(stats->stale == 0);
    CHECK(stats->drawn > 10);
    const TerrainBody& body = *bodies.find(ground);
    const auto with_triangles = static_cast<std::size_t>(
        std::ranges::count_if(body.drawn(), [](const ChunkRef& r) { return !r->mesh.indices.empty(); }));
    CHECK(with_triangles > 10);
    CHECK(meshes.mesh_count() >= with_triangles);
    const render::RenderWorld& rw = services(world).get<render::Renderer>().render_world();
    CHECK(rw.space.frame == ground.id());
    std::size_t terrain_draws = 0;
    for (const render::MeshDraw& d : rw.meshes) {
        terrain_draws += d.materials.size() == 1 && d.materials[0] == grass ? 1 : 0;
    }
    CHECK(terrain_draws == with_triangles);
    // Each draw sits at its chunk's origin relative to the render origin, in the frame's axes (render space is the
    // camera's frame).
    std::size_t placed = 0;
    for (const ChunkRef& r : body.drawn()) {
        if (r->mesh.indices.empty()) {
            continue;
        }
        const auto o = chunk_origin(r->slot.key);
        const math::vec3 expected(math::dvec3(static_cast<double>(o[0]) * 0.5, static_cast<double>(o[1]) * 0.5,
                                              static_cast<double>(o[2]) * 0.5) -
                                  rw.space.origin);
        placed += std::ranges::count_if(
                      rw.meshes, [&](const render::MeshDraw& d) { return math::vec3(d.world[3]) == expected; }) > 0
                      ? 1
                      : 0;
    }
    CHECK(placed == with_triangles);
    // Far away, the old chunks are evicted and their meshes removed.
    camera.set(Transform{.translation = math::dvec3(-400.0, 6.0, 300.0)});
    for (int i = 0; i < 60; ++i) {
        engine.frame();
    }
    CHECK(bodies.stats(ground)->stale == 0);
    // The store holds a mesh per held chunk with triangles, no more: evicted ones were removed.
    CHECK(meshes.mesh_count() <= bodies.stats(ground)->cached);
    engine.shutdown();
}
