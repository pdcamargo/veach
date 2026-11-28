// Seams sweep, throwaway: SP-13's character controller walking on SP-12's cooked LOD0 colliders across chunk
// boundaries, octant planes and the pole (seams.hpp). The only seams file that includes Jolt.

#include "seams/seams.hpp"

#include "support/jolt_runtime.hpp"  // Jolt.h first

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "sp12/sp12_jolt.hpp"

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <memory>

namespace veach::seams {

namespace {

namespace layers {
constexpr JPH::ObjectLayer static_ = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
}  // namespace layers

constexpr float dt_f = 1.0f / 60.0f;
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
JPH::Vec3 fvec(const Vec& v) {
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}
Vec dvec(JPH::RVec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
/// The shortest rotation from +Y to unit `b`, as a float quaternion (SP-13's from_to).
JPH::Quat from_y(const Vec& b) {
    const Vec c = cross({0.0, 1.0, 0.0}, b);
    const double w = 1.0 + b[1];
    const double l = std::sqrt(w * w + dot(c, c));
    if (l < 1e-12) {
        return JPH::Quat(1.0f, 0.0f, 0.0f, 0.0f);  // b = −Y: half a turn about X
    }
    return JPH::Quat(static_cast<float>(c[0] / l), static_cast<float>(c[1] / l), static_cast<float>(c[2] / l),
                     static_cast<float>(w / l))
        .Normalized();
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
    void add(JPH::Vec3Arg v) {
        add(v.GetX());
        add(v.GetY());
        add(v.GetZ());
    }
    void add(JPH::RVec3Arg v) {
        add(v.GetX());
        add(v.GetY());
        add(v.GetZ());
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

/// A chunk's collider cooked from its own triangles plus every neighbor triangle that touches its box (a vertex on
/// its boundary): welded by lattice position, so the chunk's border edges have their neighbors inside the same
/// MeshShape and Jolt's active-edge pass marks the smooth ones inactive. The ring triangles are duplicated in two
/// colliders. Vertices relative to the chunk origin (exact floats: multiples of 2^-17 m, |x| < 32 m).
/// The material of ring triangles in cook_with_ring(..., tag_ring = true): RingFilter rejects contacts with them, so
/// they only give the chunk's border edges their neighbors (Jolt's active edges) and never collide.
const JPH::PhysicsMaterial* ring_material() {
    static const JPH::RefConst<JPH::PhysicsMaterial> m = new JPH::PhysicsMaterial();
    return m.GetPtr();
}

class RingFilter final : public JPH::CharacterContactListener {
public:
    bool OnContactValidate(const JPH::CharacterVirtual*, const JPH::CharacterContact& contact) override {
        return contact.mMaterial != ring_material();
    }
};

sp12::Collider cook_with_ring(const sp12::Terrain& terrain, const voxel::ChunkKey& key, bool tag_ring) {
    sp12::Collider out;
    out.key = key;
    const auto o = voxel::chunk_origin(key);
    const std::int64_t e = voxel::chunk_units(0);
    const auto touches = [&](const std::array<std::int64_t, 3>& p) {
        bool on_face = false;
        for (std::size_t a = 0; a < 3; ++a) {
            const std::int64_t lo = o[a] << voxel::fixed_bits, hi = (o[a] + e) << voxel::fixed_bits;
            if (p[a] < lo || p[a] > hi) {
                return false;
            }
            on_face = on_face || p[a] == lo || p[a] == hi;
        }
        return on_face;
    };
    std::map<std::array<std::int64_t, 3>, JPH::uint32> weld;
    JPH::VertexList vertices;
    JPH::IndexedTriangleList triangles;
    const auto vertex = [&](const std::array<std::int64_t, 3>& p) {
        const auto [it, inserted] = weld.emplace(p, static_cast<JPH::uint32>(vertices.size()));
        if (inserted) {
            std::array<float, 3> f{};
            for (std::size_t a = 0; a < 3; ++a) {
                f[a] = static_cast<float>(static_cast<double>(p[a] - (o[a] << voxel::fixed_bits)) *
                                          (voxel::lod0_cell_meters / static_cast<double>(voxel::fixed_one)));
            }
            vertices.push_back(JPH::Float3(f[0], f[1], f[2]));
        }
        return it->second;
    };
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const sp12::Chunk* c = terrain.find({0, key.x + dx, key.y + dy, key.z + dz});
                if (c == nullptr) {
                    continue;
                }
                const voxel::ChunkMesh& m = c->mesh;
                const bool own = dx == 0 && dy == 0 && dz == 0;
                for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                    const auto& a = m.fixed[m.indices[t]];
                    const auto& b = m.fixed[m.indices[t + 1]];
                    const auto& d = m.fixed[m.indices[t + 2]];
                    if (own || touches(a) || touches(b) || touches(d)) {
                        triangles.push_back(
                            JPH::IndexedTriangle(vertex(a), vertex(b), vertex(d), own || !tag_ring ? 0u : 1u));
                    }
                }
            }
        }
    }
    if (triangles.empty()) {
        return out;
    }
    JPH::PhysicsMaterialList materials;
    materials.push_back(JPH::PhysicsMaterial::sDefault);
    materials.push_back(ring_material());
    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles), std::move(materials));
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        return out;
    }
    auto cooked = std::make_shared<sp12::CookedShape>();
    cooked->shape = result.Get();
    out.triangles = cooked->shape->GetStats().mNumTriangles;
    out.shape = std::move(cooked);
    return out;
}

}  // namespace

WalkResult run_walk(const WalkConfig& config) {
    test::ensure_jolt();
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
    JPH::ObjectLayerPairFilterTable pair_filter(layers::count);
    pair_filter.EnableCollision(layers::static_, layers::moving);
    pair_filter.EnableCollision(layers::moving, layers::moving);
    JPH::BroadPhaseLayerInterfaceTable broad_phase(layers::count, 2);
    broad_phase.MapObjectToBroadPhaseLayer(layers::static_, JPH::BroadPhaseLayer(0));
    broad_phase.MapObjectToBroadPhaseLayer(layers::moving, JPH::BroadPhaseLayer(1));
    const JPH::ObjectVsBroadPhaseLayerFilterTable object_vs_broad_phase(broad_phase, 2, pair_filter, layers::count);
    JPH::PhysicsSystem system;
    system.Init(1024, 0, 4096, 4096, broad_phase, object_vs_broad_phase, pair_filter);
    system.SetGravity(JPH::Vec3::sZero());
    JPH::TempAllocatorImpl temp(16 * 1024 * 1024);

    std::vector<voxel::ChunkKey> keys;
    for (const sp12::Chunk& c : terrain.chunks()) {
        if (!c.mesh.indices.empty()) {
            keys.push_back(c.samples.key);
        }
    }
    std::vector<sp12::Collider> colliders(keys.size());
    jobs.parallel_for("seams cook", keys.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            colliders[i] = config.ring != Ring::none ? cook_with_ring(terrain, keys[i], config.ring == Ring::adjacency)
                                                     : sp12::cook_collider(terrain.find(keys[i])->mesh);
        }
    });
    JPH::BodyInterface& bodies = system.GetBodyInterface();
    if (config.merged) {
        // The control: every chunk's triangles in one MeshShape at the scene origin, vertices welded by their
        // canonical lattice position, so the triangles are the same but no collider boundary is left between
        // chunks. The vertex floats are exact: multiples of 2^-17 m under 128 m from the origin.
        std::map<std::array<std::int64_t, 3>, JPH::uint32> weld;
        JPH::VertexList vertices;
        JPH::IndexedTriangleList triangles;
        for (const voxel::ChunkKey& k : keys) {
            const voxel::ChunkMesh& m = terrain.find(k)->mesh;
            std::vector<JPH::uint32> ids(m.fixed.size());
            for (std::size_t v = 0; v < m.fixed.size(); ++v) {
                const auto [it, inserted] = weld.emplace(m.fixed[v], static_cast<JPH::uint32>(vertices.size()));
                if (inserted) {
                    std::array<float, 3> f{};
                    for (std::size_t a = 0; a < 3; ++a) {
                        const std::int64_t rel = m.fixed[v][a] - (origin[a] << voxel::fixed_bits);
                        f[a] = static_cast<float>(static_cast<double>(rel) *
                                                  (voxel::lod0_cell_meters / static_cast<double>(voxel::fixed_one)));
                    }
                    vertices.push_back(JPH::Float3(f[0], f[1], f[2]));
                }
                ids[v] = it->second;
            }
            for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                triangles.push_back(
                    JPH::IndexedTriangle(ids[m.indices[t]], ids[m.indices[t + 1]], ids[m.indices[t + 2]], 0));
            }
        }
        JPH::MeshShapeSettings mesh(std::move(vertices), std::move(triangles));
        const JPH::ShapeSettings::ShapeResult shape = mesh.Create();
        if (!shape.HasError()) {
            bodies.CreateAndAddBody(JPH::BodyCreationSettings(shape.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                                              JPH::EMotionType::Static, layers::static_),
                                    JPH::EActivation::DontActivate);
            r.colliders = 1;
        }
    }
    for (const sp12::Collider& c : colliders) {
        if (c.shape == nullptr || config.merged) {
            continue;
        }
        const auto o = voxel::chunk_origin(c.key);
        const JPH::RVec3 p(static_cast<double>(o[0] - origin[0]) * voxel::lod0_cell_meters,
                           static_cast<double>(o[1] - origin[1]) * voxel::lod0_cell_meters,
                           static_cast<double>(o[2] - origin[2]) * voxel::lod0_cell_meters);
        bodies.CreateAndAddBody(JPH::BodyCreationSettings(c.shape->shape, p, JPH::Quat::sIdentity(),
                                                          JPH::EMotionType::Static, layers::static_),
                                JPH::EActivation::DontActivate);
        ++r.colliders;
        r.collider_triangles += c.triangles;
    }
    system.OptimizeBroadPhase();

    // The walk: from through − half_length · direction (on the surface) along the great circle, there and back.
    const Vec mid = surface_point(config.through);
    const Vec start = surface_point(add(mid, config.direction, -config.half_length));
    const Vec end = surface_point(add(mid, config.direction, config.half_length));
    Vec up = normalize(start);
    Vec forward = normalize(add(config.direction, up, -dot(config.direction, up)));
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, 0.9f, 0.0f), JPH::Quat::sIdentity(),
                                                           new JPH::CapsuleShape(0.6f, 0.3f))
                           .Create()
                           .Get();
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.3f);
    settings->mMaxSlopeAngle = static_cast<float>(45.0 * 3.141592653589793 / 180.0);
    settings->mEnhancedInternalEdgeRemoval = config.edge_removal;
    settings->mUp = fvec(up);
    const auto local = [&](const Vec& body) {
        return JPH::RVec3(body[0] - origin_m[0], body[1] - origin_m[1], body[2] - origin_m[2]);
    };
    JPH::Ref<JPH::CharacterVirtual> character =
        new JPH::CharacterVirtual(settings, local(add(start, up, 0.05)), from_y(up), &system);
    RingFilter ring_filter;
    if (config.ring == Ring::adjacency) {
        character->SetListener(&ring_filter);
    }
    const double up_alpha = 1.0 - dmath::exp(-(1.0 / 60.0) / up_tau);

    const auto feet = [&]() {
        return add(dvec(character->GetPosition()), origin_m);
    };
    /// Feet height above the colliders along −up (ray from 1 m above the feet, 3 m long).
    /// Jolt's ray test is not watertight: a ray exactly through a shared vertex or edge can miss both triangles, so
    /// a miss is retried 1 mm to the side (and counted).
    const auto gap = [&](const Vec& at, const Vec& u) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const Vec side =
                attempt == 0 ? Vec{} : scale(normalize(cross(u, attempt == 1 ? Vec{1, 0, 0} : Vec{0, 0, 1})), 1e-3);
            const JPH::RRayCast ray(local(add(add(at, side), u, 1.0)), fvec(scale(u, -3.0)));
            JPH::RayCastResult hit;
            if (system.GetNarrowPhaseQuery().CastRay(ray, hit)) {
                return 3.0 * static_cast<double>(hit.mFraction) - 1.0;
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
            // SP-13's update_character: up follows −ĝ, heading parallel-transported, ground velocity kept.
            const Vec p = feet();
            const Vec radial = normalize(p);
            const Vec g = scale(radial, -gravity);
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
            const JPH::Vec3 fup = fvec(up);
            character->SetUp(fup);
            character->SetRotation(from_y(up));
            const JPH::Vec3 velocity = character->GetLinearVelocity();
            JPH::Vec3 next = fup * velocity.Dot(fup);
            if (character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround &&
                (velocity - character->GetGroundVelocity()).Dot(fup) < 0.1f) {
                next = character->GetGroundVelocity();
            }
            next += fvec(g) * dt_f;
            if (walking) {
                next += fvec(scale(forward, walk_speed));
            }
            character->SetLinearVelocity(next);
            JPH::CharacterVirtual::ExtendedUpdateSettings update;
            update.mStickToFloorStepDown = -fup * 0.5f;
            update.mWalkStairsStepUp = fup * 0.4f;
            character->ExtendedUpdate(dt_f, fvec(g), update, system.GetDefaultBroadPhaseLayerFilter(layers::moving),
                                      system.GetDefaultLayerFilter(layers::moving), {}, {}, temp);

            const Vec now = feet();
            const Vec v = scale(add(now, prev_pos, -1.0), 60.0);
            chain.add(character->GetPosition());
            chain.add(character->GetLinearVelocity());
            chain.add(up);
            chain.add(forward);
            const auto ground = character->GetGroundState();
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
                if (ground != JPH::CharacterVirtual::EGroundState::OnGround) {
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
                    const double dv = length(add(v, prev_v, -1.0));
                    if (config.trace) {
                        const Vec d = add(v, prev_v, -1.0);
                        r.trace.push_back({now, dv, dot(d, up), near_boundary(now, 0.5)});
                    }
                    if (near_plane(now, 0.5)) {
                        ++r.plane_steps;
                        r.max_dv_plane = std::max(r.max_dv_plane, dv);
                    }
                    if (near_boundary(now, 0.5)) {
                        ++r.boundary_steps;
                        r.max_dv_boundary = std::max(r.max_dv_boundary, dv);
                        dv_boundary.push_back(dv);
                    } else {
                        r.max_dv_interior = std::max(r.max_dv_interior, dv);
                        dv_interior.push_back(dv);
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
