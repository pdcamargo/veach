// Spike SP-12 (VCH-088), throwaway: see sp12_scenario.hpp.

#include "sp12/sp12_scenario.hpp"

#include "support/sp08_field.hpp"

#include <veach/core/hash.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>

namespace veach::sp12 {

using namespace voxel;

std::vector<BrushOp> dig_log(const Site& site) {
    std::vector<BrushOp> log;
    const auto op = [&](std::uint64_t tick, BrushKind kind, const std::array<std::int64_t, 3>& center,
                        std::int32_t radius, std::uint8_t material) {
        BrushOp o;
        o.kind = kind;
        o.center_mm = center;
        o.radius_mm = radius;
        o.material = material;
        o.tick = tick;
        o.seq = log.size() + 1;
        log.push_back(o);
    };
    // Tick 90: the ball has landed and rolls about the basin.
    op(90, BrushKind::subtract, site_offset_mm(site, 0, 0, 0), 3'000, 0);  // the pit, centered on the surface
    op(90, BrushKind::subtract, site_offset_mm(site, 1'500, 500, -1'000), 2'000, 0);  // deeper, off-center
    // The ground 5 m west is 0.39 m higher (veach-bench-sp12 --surface -5000 0): the mound sits on it.
    op(90, BrushKind::add, site_offset_mm(site, -5'000, 0, 395), 1'500, 3);               // a mound of material 3
    op(90, BrushKind::subtract, site_offset_mm(site, -5'000, 0, 1'395), 800, 0);          // a dent in the mound's top
    op(90, BrushKind::subtract, site_offset_mm(site, 5'000, 10'000, -25'000), 2'500, 0);  // a cavity in rock
    // Tick second_dig_tick: the ball sleeps at the pit's bottom; a sump under it wakes it and it falls again.
    op(second_dig_tick, BrushKind::subtract, site_offset_mm(site, 0, 0, -3'200), 1'200, 0);
    return log;
}

namespace {

std::vector<physics::CookedCollider> cook_all(const Terrain& terrain, const std::vector<ChunkKey>& keys,
                                              JobSystem& jobs) {
    std::vector<physics::CookedCollider> out(keys.size());
    jobs.parallel_for("sp12 cook colliders", keys.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            out[i] = *physics::cook_collider(collider_mesh(terrain.find(keys[i])->mesh));
        }
    });
    return out;
}

/// SP-12's state hash of one body: position (double bits), rotation, linear and angular velocity (float bits).
std::uint64_t body_hash(const physics::BodyState& s) {
    const auto mix = [](std::uint64_t x) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    };
    std::uint64_t h = 14695981039346656037ULL;
    const auto add = [&](std::uint64_t bits) {
        h = mix(h ^ bits) * 1099511628211ULL;
    };
    const auto d = [&](double v) {
        add(std::bit_cast<std::uint64_t>(v));
    };
    const auto f = [&](float v) {
        add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v)));
    };
    d(s.position.x);
    d(s.position.y);
    d(s.position.z);
    f(s.rotation.x);
    f(s.rotation.y);
    f(s.rotation.z);
    f(s.rotation.w);
    f(s.linear_velocity.x);
    f(s.linear_velocity.y);
    f(s.linear_velocity.z);
    f(s.angular_velocity.x);
    f(s.angular_velocity.y);
    f(s.angular_velocity.z);
    return h;
}

/// SP-12's install hash: every terrain collider in key order, its install slot, position bits and shape hash.
std::uint64_t install_hash(const std::vector<physics::ColliderInfo>& colliders) {
    const auto mix = [](std::uint64_t x) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    };
    std::uint64_t h = 14695981039346656037ULL;
    const auto add = [&](std::uint64_t bits) {
        h = mix(h ^ bits) * 1099511628211ULL;
    };
    for (const physics::ColliderInfo& c : colliders) {
        add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.key.x)));
        add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.key.y)));
        add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.key.z)));
        add(static_cast<std::uint64_t>(c.slot));
        add(std::bit_cast<std::uint64_t>(c.position.x));
        add(std::bit_cast<std::uint64_t>(c.position.y));
        add(std::bit_cast<std::uint64_t>(c.position.z));
        add(c.shape_hash);
    }
    return h;
}

}  // namespace

ScenarioResult run_scenario(const ScenarioConfig& config) {
    ScenarioResult r;
    JobSystem jobs(config.workers);
    const PlanetField field;
    const Site& at = config.site;
    const ChunkKey center = chunk_of_mm(at.center_mm);
    Terrain terrain(field, center, config.half);
    terrain.load(jobs);
    r.base_samples_hash = terrain.samples_hash();
    r.base_mesh_hash = terrain.mesh_hash();

    std::vector<ChunkKey> meshed;
    for (const Chunk& c : terrain.chunks()) {
        if (c.sampled) {
            meshed.push_back(c.samples.key);
        }
    }
    // One body-fixed surface scene (ADR 0058): its origin is the center chunk's lattice corner, with the body-fixed
    // axes, so a chunk's position in it is an exact integer difference (ADR 0077 decision 6).
    const auto origin = chunk_origin(center);
    physics::PhysicsWorld world({.jobs = &jobs});
    const physics::SceneId scene = *world.create_scene({.name = "sp12", .lattice_origin = origin});
    {
        const std::vector<physics::CookedCollider> cooked = cook_all(terrain, meshed, jobs);
        if (!world.install_colliders(scene, cooked)) {
            return r;
        }
    }
    // Radial gravity toward the planet's center (the body-local origin), from double positions.
    world.set_gravity([](const physics::GravityQuery& q) {
        const math::dvec3& p = q.position;
        const double rr = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        return p * (-9.81 / rr);
    });

    // The dig site and up, in the scene (exact: integer millimeters minus an integer origin).
    std::array<double, 3> site{};
    for (std::size_t a = 0; a < 3; ++a) {
        site[a] = static_cast<double>(at.center_mm[a] - origin[a] * lod0_cell_mm) / 1000.0;
    }
    std::array<double, 3> start{};
    for (std::size_t a = 0; a < 3; ++a) {
        start[a] = site[a] + 2.5 * at.up[a];
    }
    // A rolling ball keeps rolling in a round pit for a long time with Jolt's damping (0.05); a dug-in rock settles.
    const physics::BodyId ball = *world.add_body(scene, {.shape = physics::Shape::sphere(0.4f),
                                                         .position = {start[0], start[1], start[2]},
                                                         .friction = 0.6f,
                                                         .restitution = 0.1f,
                                                         .linear_damping = 0.2f,
                                                         .angular_damping = 1.0f});
    const auto relative = [&](const physics::BodyState& b, double& depth, double& lateral) {
        std::array<double, 3> d{};
        for (std::size_t a = 0; a < 3; ++a) {
            d[a] = b.position[static_cast<math::length_t>(a)] - site[a];
        }
        depth = d[0] * at.up[0] + d[1] * at.up[1] + d[2] * at.up[2];
        const double l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - depth * depth;
        lateral = std::sqrt(l2 > 0.0 ? l2 : 0.0);
    };

    const std::vector<BrushOp> log = config.dig ? dig_log(at) : std::vector<BrushOp>{};
    std::size_t next_op = 0;
    std::map<ChunkKey, int> changed;
    std::uint64_t chain = hash_id("sp12:chain");
    for (int tick = 0; tick < config.steps; ++tick) {
        if (next_op < log.size() && log[next_op].tick == static_cast<std::uint64_t>(tick)) {
            const physics::BodyState state = world.state(ball);
            double lateral = 0.0;
            ScenarioResult::Batch& batch = r.batches[next_op == 0 ? 0 : 1];
            relative(state, batch.depth_before, lateral);
            batch.asleep_before = !state.awake;
            // Every op of this tick in seq order, each on its chunks in parallel; then the changed chunks' colliders.
            std::vector<ChunkKey> dirty;
            for (; next_op < log.size() && log[next_op].tick == static_cast<std::uint64_t>(tick); ++next_op) {
                for (const ChunkKey& k : terrain.apply(log[next_op], jobs, config.reverse)) {
                    dirty.push_back(k);
                    changed.emplace(k, 0);
                }
            }
            std::sort(dirty.begin(), dirty.end());
            dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
            batch.chunks = dirty.size();
            // Requested at t, installed before step t + K in key order, waking the ball (ADR 0058, 0077).
            std::vector<physics::ColliderMesh> meshes;
            for (const ChunkKey& k : dirty) {
                meshes.push_back(collider_mesh(terrain.find(k)->mesh));
            }
            if (!world.request_colliders(scene, std::move(meshes))) {
                return r;
            }
        }
        world.step();
        const std::uint64_t h = body_hash(world.state(ball));
        r.step_hashes.push_back(h);
        chain = hash(chain, h);
    }
    r.chain = chain;
    r.jolt_state = world.state_hash(scene);
    const std::vector<physics::ColliderInfo> installed = world.colliders(scene);
    r.install_hash = install_hash(installed);
    r.terrain_bodies = installed.size();

    r.samples_hash = terrain.samples_hash();
    r.mesh_hash = terrain.mesh_hash();
    std::vector<ChunkKey> all;
    for (const Chunk& c : terrain.chunks()) {
        if (c.sampled) {
            all.push_back(c.samples.key);
        }
    }
    std::vector<std::uint64_t> shape_hashes;
    for (const physics::CookedCollider& c : cook_all(terrain, all, jobs)) {
        shape_hashes.push_back(c.shape.hash());
    }
    r.colliders_hash = combine_hashes(shape_hashes);
    r.sampled = terrain.sampled_count();
    r.meshed = terrain.meshed_count();
    r.changed = changed.size();
    r.materialized = terrain.materialized_count();
    r.outside = terrain.outside_count();
    r.mismatches = terrain.shared_sample_mismatches();

    {
        // SP-08's exact crack check: every chunk's vertices welded by lattice position; an edge used once away from
        // the region's boundary is a hole.
        std::vector<sp08::MeshedChunk> all_meshes;
        for (const Chunk& c : terrain.chunks()) {
            sp08::MeshedChunk m;
            m.leaf.key = c.mesh.key;
            m.mesh = c.mesh;
            all_meshes.push_back(std::move(m));
        }
        const auto o = chunk_origin(center);
        const std::int64_t e = chunk_units(0);
        const std::array<std::int64_t, 3> lo{o[0] - config.half * e, o[1] - config.half * e, o[2] - config.half * e};
        const std::array<std::int64_t, 3> hi{o[0] + (config.half + 1) * e, o[1] + (config.half + 1) * e,
                                             o[2] + (config.half + 1) * e};
        const sp08::SeamReport seams = sp08::check_seams(all_meshes, lo, hi);
        r.open_edges = seams.open_edges;
        r.triangles = seams.triangles;
        r.wound_out = seams.wound_out;
    }
    if (config.keep_meshes) {
        for (const Chunk& c : terrain.chunks()) {
            if (!c.mesh.indices.empty()) {
                r.meshes.push_back(c.mesh);
            }
        }
        r.origin = origin;
        r.site = site;
        const math::dvec3 p = world.state(ball).position;
        r.ball = {p.x, p.y, p.z};
    }
    const physics::BodyState b = world.state(ball);
    relative(b, r.depth, r.lateral);
    r.speed = static_cast<double>(math::length(b.linear_velocity));
    return r;
}

}  // namespace veach::sp12
