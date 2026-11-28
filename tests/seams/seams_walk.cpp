// Seams sweep, throwaway: SP-13's character controller walking on SP-12's cooked LOD0 colliders across chunk
// boundaries, octant planes and the pole (seams.hpp). Runs on engine/physics since VCH-113: the adjacency ring is
// physics::with_adjacency_ring, the character is physics's, and no file here includes Jolt.

#include "seams/seams.hpp"

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/physics/physics.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <memory>

namespace veach::seams {

namespace {

constexpr double gravity = 9.81;
constexpr double walk_speed = 5.0;
constexpr double up_tau = 0.15;   ///< s: SP-13's up follows −ĝ
constexpr int stop_steps = 30;    ///< standing still at the end of a leg, then turning around
constexpr int settle_steps = 15;  ///< steps after a start that the |Δv| statistics skip (the start is intended)

double dot(const Vec& a, const Vec& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec cross(const Vec& a, const Vec& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec add(const Vec& a, const Vec& b, double s = 1.0) {
    return {a[0] + s * b[0], a[1] + s * b[1], a[2] + s * b[2]};
}
Vec scale(const Vec& a, double s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
double length(const Vec& a) {
    return std::sqrt(dot(a, a));
}
Vec normalize(const Vec& v) {
    return scale(v, 1.0 / length(v));
}
double angle(const Vec& a, const Vec& b) {
    // atan2(|a × b|, a · b), accurate for tiny angles; std:: is fine, this is a statistic, not simulation.
    return std::atan2(length(cross(a, b)), dot(a, b));
}
math::dvec3 dv(const Vec& v) {
    return {v[0], v[1], v[2]};
}
Vec vv(const math::dvec3& v) {
    return {v.x, v.y, v.z};
}
math::vec3 fv(const Vec& v) {
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}
/// The shortest rotation from +Y to unit `b` (SP-13's from_to), as the double quaternion whose float rounding the
/// character gets.
math::dquat from_y(const Vec& b) {
    const Vec c = cross({0.0, 1.0, 0.0}, b);
    const double w = 1.0 + b[1];
    const double l = std::sqrt(w * w + dot(c, c));
    if (l < 1e-12) {
        return {0.0, 1.0, 0.0, 0.0};  // b = −Y: half a turn about X
    }
    return {w / l, c[0] / l, c[1] / l, c[2] / l};
}

struct Hasher {
    std::uint64_t value = hash_id("seams:walk");
    void add(double v) { value = hash(value, std::bit_cast<std::uint64_t>(v)); }
    void add(float v) { value = hash(value, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v))); }
    void add(const Vec& v) {
        add(v[0]);
        add(v[1]);
        add(v[2]);
    }
    void add(const math::vec3& v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
    void add(const math::dvec3& v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
};

double percentile(std::vector<double> v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    const auto k = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

/// A chunk's collider mesh with its neighbors' border triangles (ADR 0083): `adjacency` tags them so they never
/// collide; `duplicate` (a measured alternative) leaves them colliding.
physics::ColliderMesh ring_mesh(const sp12::Terrain& terrain, const voxel::ChunkKey& key, bool tag_ring) {
    std::array<physics::ChunkTriangles, 27> around{};
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const sp12::Chunk* c = terrain.find({0, key.x + dx, key.y + dy, key.z + dz});
                if (c != nullptr) {
                    around[static_cast<std::size_t>((dz + 1) * 9 + (dy + 1) * 3 + (dx + 1))] = {c->mesh.fixed,
                                                                                                c->mesh.indices};
                }
            }
        }
    }
    physics::ColliderMesh mesh = physics::with_adjacency_ring(sp12::physics_key(key), voxel::chunk_origin(key),
                                                              voxel::chunk_units(0), around, voxel::fixed_bits);
    if (!tag_ring) {
        std::fill(mesh.adjacency.begin(), mesh.adjacency.end(), std::uint8_t{0});
    }
    return mesh;
}

}  // namespace

WalkResult run_walk(const WalkConfig& config) {
    WalkResult r;
    JobSystem jobs(config.workers);
    const sp12::PlanetField field;
    std::array<std::int64_t, 3> through_mm{};
    for (std::size_t a = 0; a < 3; ++a) {
        through_mm[a] = static_cast<std::int64_t>(std::floor(config.through[a] * 1000.0 + 0.5));
    }
    const voxel::ChunkKey center = sp12::chunk_of_mm(through_mm);
    sp12::Terrain terrain(field, center, config.half);
    terrain.load(jobs);

    // The scene: origin at the center chunk's minimum corner, body-fixed axes, zero world gravity (SP-12).
    const auto origin = voxel::chunk_origin(center);
    const Vec origin_m{static_cast<double>(origin[0]) * voxel::lod0_cell_meters,
                       static_cast<double>(origin[1]) * voxel::lod0_cell_meters,
                       static_cast<double>(origin[2]) * voxel::lod0_cell_meters};
    physics::PhysicsWorld world({.jobs = &jobs});
    const physics::SceneId scene = *world.create_scene({.name = "seams", .lattice_origin = origin});
    // Gravity 9.81 m/s² toward the planet's center, computed as SP-13's walker did.
    world.set_gravity([](const physics::GravityQuery& q) { return dv(scale(normalize(vv(q.position)), -gravity)); });

    std::vector<voxel::ChunkKey> keys;
    for (const sp12::Chunk& c : terrain.chunks()) {
        if (!c.mesh.indices.empty()) {
            keys.push_back(c.samples.key);
        }
    }
    std::vector<physics::CookedCollider> colliders(keys.size());
    jobs.parallel_for("seams cook", keys.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const physics::ColliderMesh mesh = config.ring != Ring::none
                                                   ? ring_mesh(terrain, keys[i], config.ring == Ring::adjacency)
                                                   : sp12::collider_mesh(terrain.find(keys[i])->mesh);
            colliders[i] = *physics::cook_collider(mesh);
        }
    });
    if (config.merged) {
        // The control: every chunk's triangles in one MeshShape at the scene origin, vertices welded by their
        // canonical lattice position, so the triangles are the same but no collider boundary is left between
        // chunks. The vertex floats are exact: multiples of 2^-17 m under 128 m from the origin.
        std::map<std::array<std::int64_t, 3>, std::uint32_t> weld;
        std::vector<math::vec3> vertices;
        std::vector<std::array<std::uint32_t, 3>> triangles;
        for (const voxel::ChunkKey& k : keys) {
            const voxel::ChunkMesh& m = terrain.find(k)->mesh;
            std::vector<std::uint32_t> ids(m.fixed.size());
            for (std::size_t v = 0; v < m.fixed.size(); ++v) {
                const auto [it, inserted] = weld.emplace(m.fixed[v], static_cast<std::uint32_t>(vertices.size()));
                if (inserted) {
                    std::array<float, 3> f{};
                    for (std::size_t a = 0; a < 3; ++a) {
                        const std::int64_t rel = m.fixed[v][a] - (origin[a] << voxel::fixed_bits);
                        f[a] = static_cast<float>(static_cast<double>(rel) *
                                                  (voxel::lod0_cell_meters / static_cast<double>(voxel::fixed_one)));
                    }
                    vertices.emplace_back(f[0], f[1], f[2]);
                }
                ids[v] = it->second;
            }
            for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                triangles.push_back({ids[m.indices[t]], ids[m.indices[t + 1]], ids[m.indices[t + 2]]});
            }
        }
        if (auto shape = physics::Shape::mesh(vertices, triangles)) {
            if (world.add_body(scene, {.shape = *shape, .motion = physics::MotionType::static_body})) {
                r.colliders = 1;
            }
        }
    } else {
        static_cast<void>(world.install_colliders(scene, colliders));
        r.colliders = colliders.size();
        for (const physics::CookedCollider& c : colliders) {
            r.collider_triangles += c.triangles;
        }
    }
    world.optimize(scene);

    // The walk: from through − half_length · direction (on the surface) along the great circle, there and back.
    const Vec mid = surface_point(config.through);
    const Vec start = surface_point(add(mid, config.direction, -config.half_length));
    const Vec end = surface_point(add(mid, config.direction, config.half_length));
    Vec up = normalize(start);
    Vec forward = normalize(add(config.direction, up, -dot(config.direction, up)));
    const auto local = [&](const Vec& body) {
        return math::dvec3(body[0] - origin_m[0], body[1] - origin_m[1], body[2] - origin_m[2]);
    };
    const physics::CharacterId character = *world.add_character(
        scene, {.shape = *physics::Shape::offset(physics::Shape::capsule(0.6f, 0.3f), math::vec3(0.0f, 0.9f, 0.0f)),
                .enhanced_internal_edge_removal = config.edge_removal,
                .position = local(add(start, up, 0.05)),
                .rotation = from_y(up),
                .up = dv(up)});
    const double up_alpha = 1.0 - dmath::exp(-(1.0 / 60.0) / up_tau);

    const auto feet = [&]() {
        return add(vv(world.character(character).position), origin_m);
    };
    /// Feet height above the colliders along −up (ray from 1 m above the feet, 3 m long).
    /// Jolt's ray test is not watertight: a ray exactly through a shared vertex or edge can miss both triangles, so
    /// a miss is retried 1 mm to the side (and counted).
    const auto gap = [&](const Vec& at, const Vec& u) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const Vec side =
                attempt == 0 ? Vec{} : scale(normalize(cross(u, attempt == 1 ? Vec{1, 0, 0} : Vec{0, 0, 1})), 1e-3);
            if (const auto hit = world.cast_ray(scene, local(add(add(at, side), u, 1.0)), fv(scale(u, -3.0)))) {
                return 3.0 * hit->fraction - 1.0;
            }
            ++r.ray_misses;
        }
        return 3.0;
    };
    const auto chunk_of = [](const Vec& p) {
        std::array<std::int64_t, 3> k{};
        for (std::size_t a = 0; a < 3; ++a) {
            k[a] = static_cast<std::int64_t>(std::floor(p[a] / 16.0));
        }
        return k;
    };
    const auto near_boundary = [](const Vec& p, double reach) {
        for (std::size_t a = 0; a < 3; ++a) {
            const double m = p[a] - 16.0 * std::floor(p[a] / 16.0);
            if (m < reach || m > 16.0 - reach) {
                return true;
            }
        }
        return false;
    };
    const auto near_plane = [](const Vec& p, double reach) {
        return std::abs(p[0]) < reach || std::abs(p[1]) < reach || std::abs(p[2]) < reach;
    };
    const auto compass = [](const Vec& f, const Vec& u) {
        const Vec e = cross({0.0, 1.0, 0.0}, u);
        const double le = length(e);
        if (le < 1e-12) {
            return 0.0;  // undefined on the axis
        }
        const Vec east = scale(e, 1.0 / le);
        const Vec north = cross(u, east);
        return std::atan2(dot(f, east), dot(f, north));
    };

    Hasher chain;
    std::vector<double> dv_interior, dv_boundary;
    const int leg_steps = static_cast<int>(std::nearbyint(2.0 * config.half_length / walk_speed * 60.0));
    Vec prev_pos = feet(), prev_v{};
    double prev_compass = compass(forward, up);
    r.min_gap = 1e30;
    r.max_gap = -1e30;
    for (int leg = 0; leg < config.legs; ++leg) {
        // A leg walks until it passes its end point (or 1.5 times the nominal steps), then stands for stop_steps.
        const Vec& target = leg % 2 == 0 ? end : start;
        Vec leg_direction{};
        int stopped = -1;
        for (int step = 0; stopped < 0 || step < stopped + stop_steps; ++step) {
            if (stopped < 0 && step > 0 &&
                (dot(add(target, feet(), -1.0), leg_direction) <= 0.0 || step >= leg_steps * 3 / 2)) {
                stopped = step;
            }
            const bool walking = stopped < 0;
            // SP-13's update_character: up follows −ĝ, heading parallel-transported, ground velocity kept (the
            // character controller of engine/physics adds g·dt and walks).
            const Vec p = feet();
            const Vec radial = normalize(p);
            up = normalize(add(up, add(radial, up, -1.0), up_alpha));
            const Vec before = forward;
            forward = normalize(add(forward, up, -dot(forward, up)));
            if (step == 0) {
                // Each leg starts by facing its end point (intended input): slopes push the walker sideways a little
                // on every leg, and without this the drift adds up until it walks off the terrain region.
                const Vec to = add(target, p, -1.0);
                forward = normalize(add(to, up, -dot(to, up)));
                leg_direction = forward;
            }
            physics::CharacterInput input{.up = dv(up), .rotation = from_y(up), .move = std::nullopt};
            if (walking) {
                input.move = dv(scale(forward, walk_speed));
            }
            world.set_input(character, input);
            world.step();

            const physics::CharacterState state = world.character(character);
            const Vec now = feet();
            const Vec v = scale(add(now, prev_pos, -1.0), 60.0);
            chain.add(state.position);
            chain.add(state.linear_velocity);
            chain.add(up);
            chain.add(forward);
            const physics::GroundState ground = state.ground;
            chain.value = hash(chain.value, static_cast<std::uint64_t>(ground));

            if (chunk_of(now) != chunk_of(prev_pos)) {
                ++r.chunk_crossings;
            }
            for (std::size_t a = 0; a < 3; ++a) {
                if ((now[a] < 0.0) != (prev_pos[a] < 0.0)) {
                    ++r.octant_crossings;
                }
            }
            r.distance += length(add(now, prev_pos, -1.0));
            r.max_from_mid = std::max(r.max_from_mid, length(add(now, mid, -1.0)));
            const double h = gap(now, up);
            r.min_gap = std::min(r.min_gap, h);
            r.max_gap = std::max(r.max_gap, h);
            r.max_up_error = std::max(r.max_up_error, angle(up, normalize(now)));
            const double c = compass(forward, up);
            if (walking && step > 0) {
                r.max_heading_step = std::max(r.max_heading_step, angle(before, forward));
                double jump = std::abs(c - prev_compass);
                jump = std::min(jump, 2.0 * 3.141592653589793 - jump);
                r.compass_jump = std::max(r.compass_jump, jump);
            }
            prev_compass = c;
            if (walking && step >= settle_steps) {
                ++r.steps;
                if (ground != physics::GroundState::on_ground) {
                    ++r.unsupported;
                    if (r.first_loss_leg < 0) {
                        r.first_loss_leg = leg;
                        r.first_loss_step = step;
                        r.first_loss_feet = now;
                        r.first_loss_gap = h;
                        r.first_loss_state = static_cast<int>(ground);
                    }
                    r.max_unsupported_gap = std::max(r.max_unsupported_gap, h);
                }
                {  // the previous step walked straight too (step - 1 >= settle_steps - 1)
                    const double dvel = length(add(v, prev_v, -1.0));
                    if (config.trace) {
                        const Vec d = add(v, prev_v, -1.0);
                        r.trace.push_back({now, dvel, dot(d, up), near_boundary(now, 0.5)});
                    }
                    if (near_plane(now, 0.5)) {
                        ++r.plane_steps;
                        r.max_dv_plane = std::max(r.max_dv_plane, dvel);
                    }
                    if (near_boundary(now, 0.5)) {
                        ++r.boundary_steps;
                        r.max_dv_boundary = std::max(r.max_dv_boundary, dvel);
                        dv_boundary.push_back(dvel);
                    } else {
                        r.max_dv_interior = std::max(r.max_dv_interior, dvel);
                        dv_interior.push_back(dvel);
                    }
                }
            }
            prev_v = v;
            prev_pos = now;
        }
    }
    r.p99_dv_interior = percentile(dv_interior, 0.99);
    r.p99_dv_boundary = percentile(dv_boundary, 0.99);
    r.end_error = length(add(feet(), start, -1.0));
    r.chain = chain.value;
    return r;
}

}  // namespace veach::seams
