// engine/physics (docs/specs/physics/spec.md): Jolt behind veach::physics. The hashes below were committed from
// macOS arm64 by the spikes that validated the design (SP-11, SP-12, SP-13 and the seams sweep) and are reproduced
// here through the engine API; Windows x64 CI checks them (ADR 0043, 0058, 0075).

#include "support/test_support.hpp"

#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/physics/module.hpp>
#include <veach/physics/physics.hpp>
#include <veach/runtime/mcp_tools.hpp>
#include <veach/runtime/terrain_colliders.hpp>
#include <veach/voxel/terrain.hpp>

#include "seams/seams.hpp"
#include "sp12/sp12_terrain.hpp"
#include "support/terrain_fields.hpp"

#include <veach/core/dmath.hpp>
#include <veach/veach.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

using namespace veach;
using namespace veach::physics;

namespace {

std::string hex(std::uint64_t v) {
    return std::format("0x{:016x}", v);
}

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/// SP-11's and SP-12's state hash: FNV-style over mixed bits.
struct Hash {
    std::uint64_t value = 14695981039346656037ULL;
    void add(std::uint64_t bits) { value = mix(value ^ bits) * 1099511628211ULL; }
    void add(double v) { add(std::bit_cast<std::uint64_t>(v)); }
    void add(float v) { add(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v))); }
    void add(const BodyState& s) {
        add(s.position.x);
        add(s.position.y);
        add(s.position.z);
        add(s.rotation.x);
        add(s.rotation.y);
        add(s.rotation.z);
        add(s.rotation.w);
        add(s.linear_velocity.x);
        add(s.linear_velocity.y);
        add(s.linear_velocity.z);
        add(s.angular_velocity.x);
        add(s.angular_velocity.y);
        add(s.angular_velocity.z);
    }
};

// ---- SP-11: a pile of 200 bodies in a bowl, at the origin and 6,371 km out (tests/sp11 until VCH-113) --------

/// A value in [-1, 1] from an integer, in steps of 1/1000 (exact integer arithmetic, one rounded division).
double unit(std::uint64_t key) {
    return static_cast<double>(static_cast<std::int64_t>(mix(key) % 2001) - 1000) / 1000.0;
}

struct PileResult {
    std::vector<std::uint64_t> step_hashes;
    std::uint64_t chain = 0;
    std::uint64_t jolt_state = 0;
    std::vector<math::dvec3> positions;  ///< relative to the offset
    std::uint64_t contacts = 0;          ///< every step's contact events, chained in the order delivered
    std::size_t contact_count = 0;
    bool contacts_sorted = true;
    int update_errors = 0;
};

enum class Ground { height_field, mesh };

/// SP-11's scene: a paraboloid bowl 63 m across (64² samples 1 m apart, heights exact floats), a column of boxes,
/// spheres and capsules 4 m above it, 600 steps. World gravity is zero; each awake body gets m·g as a force.
PileResult run_pile(std::uint32_t threads, const math::dvec3& offset, Ground ground = Ground::height_field,
                    int count = 200, int steps = 600) {
    JobSystem jobs(threads);
    PhysicsWorld world({.jobs = &jobs});
    const SceneId scene = *world.create_scene({.name = "pile",
                                               .max_bodies = static_cast<std::uint32_t>(count) + 16,
                                               .max_body_pairs = 65536,
                                               .max_contact_constraints = 65536,
                                               .temp_allocator_bytes = std::size_t{64} << 20});
    constexpr std::uint32_t n = 64;
    std::vector<float> heights(n * n);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const int dx = 2 * static_cast<int>(x) - 63;
            const int dz = 2 * static_cast<int>(z) - 63;
            heights[z * n + x] = static_cast<float>(dx * dx + dz * dz) / 512.0f;
        }
    }
    Shape bowl;
    if (ground == Ground::height_field) {
        bowl = *Shape::height_field(heights, n, math::vec3(-31.5f, 0.0f, -31.5f), math::vec3(1.0f));
    } else {
        std::vector<math::vec3> vertices;
        std::vector<std::array<std::uint32_t, 3>> triangles;
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                vertices.emplace_back(static_cast<float>(x) - 31.5f, heights[z * n + x], static_cast<float>(z) - 31.5f);
            }
        }
        for (std::uint32_t z = 0; z + 1 < n; ++z) {
            for (std::uint32_t x = 0; x + 1 < n; ++x) {
                const std::uint32_t i = z * n + x;
                triangles.push_back({i, i + n, i + 1});
                triangles.push_back({i + 1, i + n, i + n + 1});
            }
        }
        bowl = *Shape::mesh(vertices, triangles);
    }
    REQUIRE(world.add_body(scene, {.shape = bowl, .motion = MotionType::static_body, .position = offset}));
    const Shape shapes[] = {Shape::box(math::vec3(0.4f, 0.3f, 0.5f)), Shape::sphere(0.45f), Shape::capsule(0.4f, 0.3f)};
    int side = 6;
    while (side * side * 10 < count) {
        ++side;
    }
    std::vector<BodyId> ids;
    std::vector<double> weights;  // SP-11's force: −g / inverse mass, in float
    for (int i = 0; i < count; ++i) {
        const auto key = static_cast<std::uint64_t>(i) * 16;
        const int layer = i / (side * side);
        const int cell = i % (side * side);
        const double half = static_cast<double>(side - 1) / 2.0;
        const math::dvec3 local((static_cast<double>(cell % side) - half) * 1.5 + 0.1 * unit(key + 0),
                                4.0 + 1.5 * static_cast<double>(layer) + 0.1 * unit(key + 1),
                                (static_cast<double>(cell / side) - half) * 1.5 + 0.1 * unit(key + 2));
        const math::dquat rotation(1.5, static_cast<double>(static_cast<float>(unit(key + 3))),
                                   static_cast<double>(static_cast<float>(unit(key + 4))),
                                   static_cast<double>(static_cast<float>(unit(key + 5))));
        auto id = world.add_body(scene, {.shape = shapes[i % 3],
                                         .position = offset + local,
                                         .rotation = rotation,
                                         .angular_velocity = {static_cast<double>(static_cast<float>(unit(key + 6))),
                                                              static_cast<double>(static_cast<float>(unit(key + 7))),
                                                              static_cast<double>(static_cast<float>(unit(key + 8)))},
                                         .friction = 0.5f,
                                         .restitution = 0.1f});
        REQUIRE(id);
        ids.push_back(*id);
        weights.push_back(static_cast<double>(-9.81f / static_cast<float>(world.inverse_mass(*id))));
    }
    world.optimize(scene);

    PileResult out;
    Hash chain;
    for (int step = 0; step < steps; ++step) {
        for (std::size_t b = 0; b < ids.size(); ++b) {
            if (world.awake(ids[b])) {
                world.add_force(ids[b], {0.0, weights[b], 0.0});
            }
        }
        world.step();
        Hash state;
        for (const BodyId id : ids) {
            BodyState s = world.state(id);
            s.position -= offset;
            state.add(s);
        }
        out.step_hashes.push_back(state.value);
        const std::span<const Contact> events = world.contacts();
        for (std::size_t i = 0; i < events.size(); ++i) {
            const Contact& c = events[i];
            Hash h;
            h.add(static_cast<std::uint64_t>(c.kind));
            h.add(static_cast<std::uint64_t>(c.a));
            h.add(static_cast<std::uint64_t>(c.b));
            h.add(static_cast<std::uint64_t>(c.sub_shape_a));
            h.add(static_cast<std::uint64_t>(c.sub_shape_b));
            h.add(c.point.x);
            h.add(c.normal.y);
            out.contacts = veach::hash(out.contacts, h.value);
            if (i > 0) {
                const Contact& p = events[i - 1];
                out.contacts_sorted = out.contacts_sorted &&
                                      std::tuple(static_cast<std::uint32_t>(p.a), static_cast<std::uint32_t>(p.b),
                                                 p.sub_shape_a, p.sub_shape_b) <=
                                          std::tuple(static_cast<std::uint32_t>(c.a), static_cast<std::uint32_t>(c.b),
                                                     c.sub_shape_a, c.sub_shape_b);
            }
            out.contacts_sorted =
                out.contacts_sorted && static_cast<std::uint32_t>(c.a) < static_cast<std::uint32_t>(c.b);
        }
        out.contact_count += events.size();
        chain.add(state.value);
    }
    out.chain = chain.value;
    out.jolt_state = world.state_hash(scene);
    for (const BodyId id : ids) {
        out.positions.push_back(world.state(id).position - offset);
    }
    return out;
}

constexpr int pile_snapshots[] = {1, 60, 300, 600};
struct PileExpected {
    std::uint64_t snapshots[std::size(pile_snapshots)];
    std::uint64_t chain;
};
// SP-11's committed values (tests/sp11/sp11_jolt_tests.cpp before VCH-113). Regenerate only on purpose.
constexpr PileExpected pile_origin = {
    {0x90f546172584bb1fULL, 0x89a2820b9fb3be9eULL, 0x79d47a6ec4279409ULL, 0x87bdb4156d528362ULL},
    0x171ce203da644de9ULL};
constexpr PileExpected pile_offset = {
    {0x82536d1fc1f7bec7ULL, 0xd756aa6a7898d1f0ULL, 0x8e0eb4048226ac7fULL, 0x364bc02eeecdeb99ULL},
    0x48bceab5bca8cea8ULL};

void check_pile(const PileResult& r, const PileExpected& e) {
    REQUIRE(r.step_hashes.size() == 600);
    for (std::size_t i = 0; i < std::size(pile_snapshots); ++i) {
        const auto step = static_cast<std::size_t>(pile_snapshots[i]);
        INFO(std::format("state hash after step {}: {}", step, hex(r.step_hashes[step - 1])));
        CHECK(r.step_hashes[step - 1] == e.snapshots[i]);
    }
    INFO("chain " << hex(r.chain));
    CHECK(r.chain == e.chain);
    for (const math::dvec3& p : r.positions) {  // the pile collided and settled in the bowl
        CHECK(p.x * p.x + p.z * p.z < 31.0 * 31.0);
        CHECK(p.y > -0.5);
        CHECK(p.y < 16.0);
    }
}

void check_same(const PileResult& a, const PileResult& b) {
    CHECK(a.step_hashes == b.step_hashes);
    CHECK(a.jolt_state == b.jolt_state);
    // [physics.AC7] Contact events are sorted and the same, in the same order, on any number of threads.
    CHECK(a.contact_count > 1000);
    CHECK(a.contacts_sorted);
    CHECK(b.contacts_sorted);
    CHECK(a.contacts == b.contacts);
}

}  // namespace

TEST_CASE(
    "[physics.AC2] [physics.AC7] SP-11's pile gives its committed hashes through the engine, on 1 and 5 job threads") {
    SUBCASE("at the origin") {
        const PileResult one = run_pile(1, math::dvec3(0.0));
        check_pile(one, pile_origin);
        check_same(one, run_pile(5, math::dvec3(0.0)));
    }
    SUBCASE("6,371 km out") {
        const math::dvec3 offset(3'822'600.0, 5'096'800.0, 0.0);
        const PileResult one = run_pile(1, offset);
        check_pile(one, pile_offset);
        check_same(one, run_pile(5, offset));
    }
}

// ---- Terrain colliders: the install queue ----------------------------------------------------------------------

namespace {

/// A flat square of 2 triangles at height `y` (lattice units) across a 32-unit chunk at `key`, fixed point.
ColliderMesh flat_chunk(const ChunkKey& key, std::int64_t y) {
    ColliderMesh m;
    m.key = key;
    m.origin = {key.x * 32, key.y * 32, key.z * 32};
    const auto v = [&](std::int64_t x, std::int64_t z) {
        return std::array<std::int64_t, 3>{(m.origin[0] + x) << 16, (m.origin[1] + y) << 16, (m.origin[2] + z) << 16};
    };
    m.vertices = {v(0, 0), v(32, 0), v(0, 32), v(32, 32)};
    m.indices = {0, 2, 1, 1, 2, 3};  // counter-clockwise seen from +Y
    return m;
}

}  // namespace

TEST_CASE("[physics.AC3] colliders requested at tick t are installed before step t + 2, in key order, and wake") {
    PhysicsWorld world;
    const SceneId free = *world.create_scene({.name = "free"});
    CHECK(world.request_colliders(free, {flat_chunk({}, 0)}).error().code == ErrorCode::physics_not_on_lattice);
    const SceneId scene = *world.create_scene({.name = "pad", .lattice_origin = LatticePoint{0, 0, 0}});
    world.set_gravity([](const GravityQuery&) { return math::dvec3(0.0, -9.81, 0.0); });
    const BodyId ball = *world.add_body(scene, {.shape = Shape::sphere(0.5f), .position = {8.0, 3.0, 8.0}});

    // Two chunks requested out of key order, then the first again with a different height: installed together at
    // t + 2, sorted by key, the later request for a chunk winning.
    REQUIRE(world.request_colliders(scene, {flat_chunk({.x = 1}, 0), flat_chunk({}, 0)}));
    REQUIRE(world.request_colliders(scene, {flat_chunk({}, 2)}));
    CHECK(world.scene_info(scene).pending_colliders == 3);
    world.step();  // tick 0 -> 1
    world.step();  // tick 1 -> 2
    CHECK(world.colliders(scene).empty());
    world.step();  // the step of tick 2 installs them first
    const std::vector<ColliderInfo> installed = world.colliders(scene);
    REQUIRE(installed.size() == 2);
    CHECK(installed[0].key == ChunkKey{});
    CHECK(installed[1].key == ChunkKey{.x = 1});
    CHECK(installed[1].position == math::dvec3(16.0, 0.0, 0.0));  // (origin − scene origin) × 0.5 m, exact
    CHECK(installed[0].slot < installed[1].slot);                 // installed in key order
    CHECK(installed[0].shape_hash == cook_collider(flat_chunk({}, 2))->shape.hash());

    // The ball lands on the raised chunk (1 m up) and falls asleep; swapping its collider in place keeps the body and
    // wakes the ball, which falls to the new height.
    for (int i = 0; i < 600 && world.awake(ball); ++i) {
        world.step();
    }
    REQUIRE_FALSE(world.awake(ball));
    CHECK(world.state(ball).position.y == doctest::Approx(1.5).epsilon(0.01));
    REQUIRE(world.request_colliders(scene, {flat_chunk({}, 0)}));
    world.step();
    world.step();
    CHECK_FALSE(world.awake(ball));
    world.step();
    CHECK(world.awake(ball));
    CHECK(world.colliders(scene)[0].slot == installed[0].slot);  // replaced in place
    for (int i = 0; i < 120; ++i) {
        world.step();
    }
    CHECK(world.state(ball).position.y == doctest::Approx(0.5).epsilon(0.02));

    // An empty mesh removes the chunk's collider.
    REQUIRE(world.request_colliders(scene, {ColliderMesh{.key = {.x = 1}, .origin = {32, 0, 0}}}));
    world.step();
    world.step();
    world.step();
    CHECK(world.colliders(scene).size() == 1);

    // A malformed mesh fails to cook with a hint.
    ColliderMesh bad = flat_chunk({.z = 1}, 0);
    bad.indices.push_back(9);
    CHECK(cook_collider(bad).error().code == ErrorCode::physics_shape_invalid);
}

TEST_CASE("[physics.AC5] the adjacency ring takes neighbors' border triangles and they never collide") {
    // Two flat chunks side by side: the ring of chunk 0 holds chunk 1's triangles that touch its +X face.
    const ColliderMesh a = flat_chunk({}, 0), b = flat_chunk({.x = 1}, 0);
    std::array<ChunkTriangles, 27> around{};
    around[13] = {a.vertices, a.indices};
    around[14] = {b.vertices, b.indices};  // dx = +1
    const ColliderMesh ring = with_adjacency_ring({}, a.origin, 32, around);
    CHECK(ring.indices.size() == 12);
    CHECK(ring.vertices.size() == 6);  // welded on the shared edge
    CHECK(ring.adjacency == std::vector<std::uint8_t>{0, 0, 1, 1});

    // A ball over ring triangles only (chunk 1's area, in chunk 0's collider) falls through them.
    PhysicsWorld world;
    const SceneId scene = *world.create_scene({.name = "ring", .lattice_origin = LatticePoint{0, 0, 0}});
    world.set_gravity([](const GravityQuery&) { return math::dvec3(0.0, -9.81, 0.0); });
    const std::array cooked{*cook_collider(ring)};
    REQUIRE(world.install_colliders(scene, cooked));
    const BodyId over_ring = *world.add_body(scene, {.shape = Shape::sphere(0.5f), .position = {24.0, 1.0, 8.0}});
    const BodyId over_own = *world.add_body(scene, {.shape = Shape::sphere(0.5f), .position = {8.0, 1.0, 8.0}});
    for (int i = 0; i < 120; ++i) {
        world.step();
    }
    CHECK(world.state(over_own).position.y == doctest::Approx(0.5).epsilon(0.02));
    CHECK(world.state(over_ring).position.y < -5.0);
}

// ---- Gravity, scenes and transfers --------------------------------------------------------------------------

TEST_CASE("[physics.AC6] the gravity hook runs for awake dynamic bodies and characters, never for sleeping ones") {
    PhysicsWorld world;
    const SceneId scene = *world.create_scene({.name = "hook"});
    int calls = 0;
    world.set_gravity([&](const GravityQuery& q) {
        ++calls;
        CHECK(q.scene == scene);
        return math::dvec3(0.0, -9.81, 0.0);
    });
    REQUIRE(world.add_body(scene, {.shape = Shape::box(math::vec3(10.0f, 0.5f, 10.0f)),
                                   .motion = MotionType::static_body,
                                   .position = {0.0, -0.5, 0.0}}));
    const BodyId box = *world.add_body(scene, {.shape = Shape::box(math::vec3(0.5f)), .position = {0.0, 0.6, 0.0}});
    REQUIRE(world.add_body(scene, {.shape = Shape::sphere(0.2f), .position = {3.0, 5.0, 0.0}, .gravity = false}));
    world.step();
    CHECK(calls == 1);  // the box only: static bodies and gravity = false take none
    for (int i = 0; i < 600 && world.awake(box); ++i) {
        world.step();
    }
    REQUIRE_FALSE(world.awake(box));
    const int asleep = calls;
    world.step();
    CHECK(calls == asleep);
    CHECK(world.mass(box) == doctest::Approx(1000.0));  // 1 m³ at Jolt's 1000 kg/m³

    const CharacterId walker =
        *world.add_character(scene, {.shape = *Shape::offset(Shape::capsule(0.6f, 0.3f), math::vec3(0.0f, 0.9f, 0.0f)),
                                     .position = {-3.0, 0.05, 0.0}});
    world.step();
    CHECK(calls == asleep + 1);
    CHECK(world.characters(scene) == std::vector{walker});
}

TEST_CASE("[physics.AC4] re-anchors, re-centering and transfers convert in double and keep ids") {
    PhysicsWorld world;
    const SceneId surface = *world.create_scene({.name = "surface", .origin = {1.0e6, 2.0e6, 0.0}});
    const SceneId flight = *world.create_scene({.name = "flight", .kind = SceneKind::free_flight});
    const BodyId crate = *world.add_body(surface, {.shape = Shape::box(math::vec3(0.5f)), .position = {3.0, 4.0, 5.0}});
    const BodyMotion before = world.motion(crate);
    CHECK(before.position == math::dvec3(1.0e6 + 3.0, 2.0e6 + 4.0, 5.0));

    // A re-anchor moves the scene, not the body.
    world.set_origin(surface, math::dvec3(1.0e6 + 1.0, 2.0e6 + 1.0, 1.0));
    CHECK(world.state(crate).position == math::dvec3(2.0, 3.0, 4.0));
    CHECK(world.motion(crate).position == before.position);

    // Into free flight, re-centered on the body: exactly zero offsets, and the origin moves on exact tick time.
    const BodyMotion fast{.position = {7.0e6, 0.0, 0.0}, .velocity = {0.0, 7'800.0, 0.0}};
    REQUIRE(world.transfer(crate, flight, fast, {.recenter = true}));
    CHECK(world.scene_of(crate) == flight);
    CHECK(world.state(crate).position == math::dvec3(0.0));
    CHECK(world.state(crate).linear_velocity == math::vec3(0.0f));
    for (int i = 0; i < 60; ++i) {
        world.step();
    }
    const SceneInfo info = world.scene_info(flight);
    CHECK(world.origin_at(flight) == info.origin + info.velocity * (60.0 / 60.0));
    CHECK(world.motion(crate).velocity == math::dvec3(0.0, 7'800.0, 0.0));  // no gravity hook: it coasts
    world.recenter(flight, crate);
    CHECK(world.state(crate).position == math::dvec3(0.0));

    // A frame change needs an empty destination.
    const BodyId other = *world.add_body(flight, {.shape = Shape::sphere(1.0f), .position = {100.0, 0.0, 0.0}});
    const auto refused =
        world.transfer(crate, flight, world.motion(crate), {.frame = SceneFrame{.frame = 7}, .recenter = true});
    CHECK(refused.error().code == ErrorCode::physics_invalid_transfer);
    world.remove_body(other);
    CHECK_FALSE(world.contains(other));
}

TEST_CASE("[physics.AC4] fictitious forces apply in rotating free flight only") {
    // A body at rest in a frame spinning at ω, 1000 km from the axis: free flight sees ω² r outward (centrifugal);
    // the surface scene does not (ADR 0058, 0080).
    const double w = 1.0e-3;
    PhysicsWorld world;
    const SceneFrame spinning{.frame = 1, .angular_velocity = {0.0, w, 0.0}};
    const SceneId flight = *world.create_scene(
        {.name = "flight", .kind = SceneKind::free_flight, .frame = spinning, .origin = {1.0e6, 0.0, 0.0}});
    const SceneId surface = *world.create_scene({.name = "surface", .frame = spinning, .origin = {1.0e6, 0.0, 0.0}});
    world.set_gravity([](const GravityQuery&) { return math::dvec3(0.0); });
    const BodySettings probe{
        .shape = Shape::sphere(1.0f), .linear_damping = 0.0f, .mass = 1.0, .allow_sleeping = false};
    const BodyId a = *world.add_body(flight, probe);
    const BodyId b = *world.add_body(surface, probe);
    world.step();
    CHECK(world.motion(a).velocity.x == doctest::Approx(w * w * 1.0e6 / 60.0).epsilon(1e-4));
    CHECK(world.motion(b).velocity.x == 0.0);
}

TEST_CASE("[physics.AC8] a character walks on a plane whose up is not +Y") {
    PhysicsWorld world;
    const SceneId scene = *world.create_scene({.name = "tilted"});
    const math::dvec3 up = math::normalize(math::dvec3(1.0, 1.0, 0.0));
    world.set_gravity([&](const GravityQuery&) { return up * -9.81; });
    const math::dvec3 axis = math::cross(math::dvec3(0.0, 1.0, 0.0), up);  // the rotation taking +Y to up
    const math::dquat tilt = math::normalize(math::dquat(1.0 + up.y, axis.x, axis.y, axis.z));
    REQUIRE(world.add_body(scene, {.shape = Shape::box(math::vec3(30.0f, 0.5f, 30.0f)),
                                   .motion = MotionType::static_body,
                                   .position = up * -0.5,
                                   .rotation = tilt}));
    const CharacterId c =
        *world.add_character(scene, {.shape = *Shape::offset(Shape::capsule(0.6f, 0.3f), math::vec3(0.0f, 0.9f, 0.0f)),
                                     .position = up * 0.05,
                                     .rotation = tilt,
                                     .up = up});
    const math::dvec3 forward(0.0, 0.0, -1.0);  // along the plane
    for (int i = 0; i < 120; ++i) {
        world.set_input(c, {.up = up, .rotation = tilt, .move = forward * 3.0});
        world.step();
    }
    const CharacterState s = world.character(c);
    CHECK(s.ground == GroundState::on_ground);
    CHECK(std::abs(math::dot(s.position, up)) < 0.02);
    CHECK(s.position.z == doctest::Approx(-6.0).epsilon(0.05));
}

// ---- The ECS binding and the MCP tools ------------------------------------------------------------------------

TEST_CASE("[physics.AC9] [physics.AC10] rigid bodies on entities step in PostSimulation, MCP tools inspect them") {
    test::TempProject project("physics-module");
    project.write("veach.project.json", R"({"name": "physics"})");
    test::LogCapture logs;
    Engine engine(test::headless_config(project.path(), {}, 0));
    engine.import<PhysicsModule>();
    REQUIRE(engine.init().has_value());
    World& world = engine.world();
    physics::PhysicsWorld& physics = physics_world(world);
    physics.set_gravity([](const GravityQuery& q) {
        return q.frame != 0 ? -math::normalize(q.position) * 9.81 : math::dvec3(0.0, -9.81, 0.0);
    });

    // The root world's default scene: a crate falls onto a static slab.
    world.entity("slab")
        .set(Transform{.translation = {0.0, -0.5, 0.0}})
        .set(RigidBody{.motion = MotionType::static_body, .half_extents = math::vec3(10.0f, 0.5f, 10.0f)});
    const Entity crate = world.entity("crate").set(Transform{.translation = {0.0, 3.0, 0.0}}).set(RigidBody{});
    // A scene in a planet's body-fixed frame, 6,371 km out: the ball's Transform is relative to the frame.
    const Entity planet =
        world.entity("planet").set(ReferenceFrame{.kind = FrameKind::rotating, .up = FrameUp::radial});
    planet.set(PhysicsScene{.kind = SceneKind::surface});
    const Entity ball = world.entity("ball")
                            .child_of(planet)
                            .set(Transform{.translation = {6.371e6 + 2.0, 0.0, 0.0}})
                            .set(RigidBody{.shape = BodyShape::sphere});
    REQUIRE(crate.has<PhysicsBody>());
    REQUIRE(ball.has<PhysicsBody>());
    CHECK(ball.get<PhysicsBody>().scene == planet.get<PhysicsSceneId>().id);
    CHECK(crate.get<PhysicsBody>().scene == static_cast<std::uint32_t>(default_scene));

    for (int i = 0; i < 120; ++i) {
        REQUIRE(engine.frame());
    }
    CHECK(physics.tick() == 120);
    CHECK(crate.get<Transform>().translation.y == doctest::Approx(0.5).epsilon(0.02));
    CHECK(ball.get<Transform>().translation.x < 6.371e6 - 10.0);  // falling toward the frame's origin
    CHECK(ball.get<FrameTransform>().frame == planet.id());

    // Inspection over MCP.
    McpTools& tools = engine.services().get<McpTools>();
    auto scenes = tools.call("physics_scenes", world, nlohmann::json::object());
    REQUIRE(scenes.has_value());
    const nlohmann::json& s = std::get<nlohmann::json>(*scenes);
    CHECK(s["imported"] == true);
    CHECK(s["scenes"].size() == 2);
    CHECK(s["scenes"][1]["name"] == "planet");
    auto bodies = tools.call("physics_bodies", world, {{"scene", 0}});
    REQUIRE(bodies.has_value());
    CHECK(std::get<nlohmann::json>(*bodies)["bodies"].size() == 2);
    CHECK(tools.call("physics_bodies", world, {{"scene", 9}}).error().code == ErrorCode::mcp_invalid_arguments);
    REQUIRE(tools.call("physics_contacts", world, nlohmann::json::object()).has_value());

    // Removing the component removes the body; a body whose parent is not its frame is refused with a hint.
    crate.remove<RigidBody>();
    CHECK_FALSE(crate.has<PhysicsBody>());
    CHECK(physics.bodies(default_scene).size() == 1);
    const Entity holder = world.entity("holder").child_of(planet).set(Transform{});
    world.entity("nested").child_of(holder).set(Transform{}).set(RigidBody{});
    CHECK(logs.has_code("physics.invalid_transfer"));
    engine.shutdown();
}

// ---- Rigid bodies across chunk borders (ADR 0083: measured here before the ring is final for them) -----------

namespace {

enum class Colliders { chunks, ring, merged };

struct Roll {
    std::vector<double> border, interior;  ///< per-step |Δv| of every ball, m/s, near a chunk border plane or not
    std::uint64_t chain = 0;
    double p99_border = 0.0, p99_interior = 0.0, max_border = 0.0;
};

double p99(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    const auto k = static_cast<std::size_t>(0.99 * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

/// Eight balls rolled at 6 m/s over SP-12's terrain, 12 m around its dig site, for 4 s, on per-chunk colliders, on
/// colliders with adjacency rings or on one merged collider.
Roll roll(const sp12::Terrain& terrain, Colliders mode, bool boxes) {
    using sp12::default_site;
    const voxel::ChunkKey center = terrain.center();
    const LatticePoint origin = voxel::chunk_origin(center);
    const math::dvec3 origin_m =
        math::dvec3(static_cast<double>(origin[0]), static_cast<double>(origin[1]), static_cast<double>(origin[2])) *
        voxel::lod0_cell_meters;
    PhysicsWorld world;
    const SceneId scene = *world.create_scene({.name = "roll", .lattice_origin = origin});
    world.set_gravity([](const GravityQuery& q) { return math::normalize(q.position) * -9.81; });
    std::vector<CookedCollider> cooked;
    std::map<std::array<std::int64_t, 3>, std::uint32_t> weld;
    std::vector<math::vec3> merged_vertices;
    std::vector<std::array<std::uint32_t, 3>> merged_triangles;
    for (const sp12::Chunk& c : terrain.chunks()) {
        if (c.mesh.indices.empty()) {
            continue;
        }
        const voxel::ChunkKey& k = c.samples.key;
        if (mode == Colliders::merged) {
            std::vector<std::uint32_t> ids(c.mesh.fixed.size());
            for (std::size_t v = 0; v < c.mesh.fixed.size(); ++v) {
                const auto [it, inserted] =
                    weld.emplace(c.mesh.fixed[v], static_cast<std::uint32_t>(merged_vertices.size()));
                if (inserted) {
                    math::vec3 f(0.0f);
                    for (std::size_t a = 0; a < 3; ++a) {
                        f[static_cast<math::length_t>(a)] = static_cast<float>(
                            static_cast<double>(c.mesh.fixed[v][a] - (origin[a] << 16)) * (0.5 / 65536.0));
                    }
                    merged_vertices.push_back(f);
                }
                ids[v] = it->second;
            }
            for (std::size_t t = 0; t + 2 < c.mesh.indices.size(); t += 3) {
                merged_triangles.push_back(
                    {ids[c.mesh.indices[t]], ids[c.mesh.indices[t + 1]], ids[c.mesh.indices[t + 2]]});
            }
            continue;
        }
        ColliderMesh mesh = sp12::collider_mesh(c.mesh);
        if (mode == Colliders::ring) {
            std::array<ChunkTriangles, 27> around{};
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (const sp12::Chunk* n = terrain.find({0, k.x + dx, k.y + dy, k.z + dz})) {
                            around[static_cast<std::size_t>((dz + 1) * 9 + (dy + 1) * 3 + (dx + 1))] = {
                                n->mesh.fixed, n->mesh.indices};
                        }
                    }
                }
            }
            mesh = with_adjacency_ring(mesh.key, mesh.origin, mesh.extent, around);
        }
        cooked.push_back(*cook_collider(mesh));
    }
    if (mode == Colliders::merged) {
        REQUIRE(world.add_body(
            scene, {.shape = *Shape::mesh(merged_vertices, merged_triangles), .motion = MotionType::static_body}));
    } else {
        REQUIRE(world.install_colliders(scene, cooked));
    }
    world.optimize(scene);

    const auto at = [](std::int64_t mm) {
        return static_cast<double>(mm) / 1000.0;
    };
    const math::dvec3 site(at(default_site.center_mm[0]), at(default_site.center_mm[1]), at(default_site.center_mm[2]));
    const math::dvec3 east(default_site.east[0], default_site.east[1], default_site.east[2]);
    const math::dvec3 north(default_site.north[0], default_site.north[1], default_site.north[2]);
    std::vector<BodyId> balls;
    for (int k = 0; k < 8; ++k) {
        const double a = static_cast<double>(k) * (3.141592653589793 / 4.0);
        const math::dvec3 dir = east * dmath::cos(a) + north * dmath::sin(a);
        const seams::Vec p =
            seams::surface_point({site.x + 12.0 * dir.x, site.y + 12.0 * dir.y, site.z + 12.0 * dir.z});
        const math::dvec3 ground(p[0], p[1], p[2]);
        const math::dvec3 up = math::normalize(ground);
        const math::dvec3 along = math::normalize(math::cross(up, dir));
        const math::dvec3 axis = math::cross(math::dvec3(0.0, 1.0, 0.0), up);
        balls.push_back(
            *world.add_body(scene, {.shape = boxes ? Shape::box(math::vec3(0.4f)) : Shape::sphere(0.4f),
                                    .position = ground + up * 0.45 - origin_m,
                                    .rotation = math::normalize(math::dquat(1.0 + up.y, axis.x, axis.y, axis.z)),
                                    .linear_velocity = along * 6.0,
                                    .friction = boxes ? 0.1f : 0.6f,
                                    .allow_sleeping = false}));
    }
    const auto near_border = [](const math::dvec3& p) {
        for (int a = 0; a < 3; ++a) {
            const double m = p[a] - 16.0 * std::floor(p[a] / 16.0);
            if (m < 0.5 || m > 15.5) {
                return true;
            }
        }
        return false;
    };
    Roll r;
    std::vector<math::dvec3> previous(balls.size());
    for (int step = 0; step < 240; ++step) {
        world.step();
        for (std::size_t b = 0; b < balls.size(); ++b) {
            const BodyMotion m = world.motion(balls[b]);
            r.chain = veach::hash(r.chain, std::bit_cast<std::uint64_t>(m.position.x));
            if (step >= 15) {
                // The kick along up: what an edge bump does (rolling and sliding change the rest).
                const double dv = std::abs(math::dot(m.velocity - previous[b], math::normalize(m.position)));
                const math::dvec3 bottom = m.position - math::normalize(m.position) * 0.4;
                (near_border(bottom) ? r.border : r.interior).push_back(dv);
            }
            previous[b] = m.velocity;
        }
    }
    r.p99_border = p99(r.border);
    r.p99_interior = p99(r.interior);
    r.max_border = r.border.empty() ? 0.0 : *std::max_element(r.border.begin(), r.border.end());
    return r;
}

}  // namespace

// Regenerate only on purpose (replay tier, cross-platform deterministic Jolt): the ring runs' position chains.
constexpr std::uint64_t ring_balls_expected = 0x1f7dfb7150479852ULL;
constexpr std::uint64_t ring_boxes_expected = 0x03186a98e4b59f08ULL;

TEST_CASE("[physics.AC5] rigid bodies cross chunk borders on adjacency-ring colliders as on one merged mesh") {
    JobSystem jobs(0);
    const sp12::PlanetField field;
    sp12::Terrain terrain(field, sp12::chunk_of_mm(sp12::default_site.center_mm), 2);
    terrain.load(jobs);
    for (const bool boxes : {false, true}) {
        const Roll chunks = roll(terrain, Colliders::chunks, boxes);
        const Roll ring = roll(terrain, Colliders::ring, boxes);
        const Roll merged = roll(terrain, Colliders::merged, boxes);
        for (const auto& [name, r] :
             {std::pair{"chunks", &chunks}, std::pair{"ring", &ring}, std::pair{"merged", &merged}}) {
            MESSAGE(std::format(
                "{} {}: |dv.up| near borders p99 {:.4f} max {:.4f} ({} steps), interior p99 {:.4f} "
                "max {:.4f} ({} steps)",
                boxes ? "boxes" : "balls", name, r->p99_border, r->max_border, r->border.size(), r->p99_interior,
                r->interior.empty() ? 0.0 : *std::max_element(r->interior.begin(), r->interior.end()),
                r->interior.size()));
        }
        CHECK(ring.border.size() > 100);
        if (boxes) {
            // Sliding boxes kick up at per-chunk border edges (p99 0.63 m/s against 0.33 inside); with the ring the
            // borders are no worse than inside, and no worse than on the merged mesh (0.20 against 0.31).
            CHECK(chunks.p99_border > 1.5 * chunks.p99_interior);
            CHECK(ring.p99_border < ring.p99_interior + 0.05);
            CHECK(ring.p99_border < merged.p99_border + 0.05);
        } else {
            // Rolling balls show no border effect with any of the three (0.32 to 0.34 m/s, inside 0.36 to 0.38).
            CHECK(chunks.p99_border < chunks.p99_interior + 0.05);
            CHECK(ring.p99_border < ring.p99_interior + 0.05);
        }
        INFO(std::format("chains: chunks {} ring {} merged {}", hex(chunks.chain), hex(ring.chain), hex(merged.chain)));
        CHECK(ring.chain == (boxes ? ring_boxes_expected : ring_balls_expected));
    }
}

// ---- The voxel bridge (VCH-114's terrain bodies, through runtime's TerrainColliders) --------------------------

TEST_CASE("[physics.AC3] a terrain body's collider updates install through TerrainColliders, and a ball lands") {
    const auto hills = test::flat_hills();
    voxel::TerrainSettings settings;
    settings.octree = {.root_level = 4, .root_min = {-2, -1, -2}, .root_max = {1, 0, 1}};
    settings.asynchronous = false;
    voxel::TerrainBody body(hills, settings);
    JobSystem jobs(4);
    PhysicsWorld world({.jobs = &jobs});
    const SceneId scene = *world.create_scene({.name = "hills", .lattice_origin = LatticePoint{0, 0, 0}});
    world.set_gravity([](const GravityQuery&) { return math::dvec3(0.0, -9.81, 0.0); });
    TerrainColliders colliders(world, scene);
    body.set_collider_sink(&colliders);

    // The ground under (8, y, 8) m, and a ball 3 m above it.
    double lo = -20.0, hi = 20.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        (hills->distance(8.0, mid, 8.0) < 0.0 ? lo : hi) = mid;
    }
    const BodyId ball = *world.add_body(scene, {.shape = Shape::sphere(0.4f), .position = {8.0, lo + 3.0, 8.0}});
    const voxel::ColliderProbe probe{.position_m = {8.0, lo + 3.0, 8.0}};
    const voxel::ColliderStats stats =
        body.update_colliders(std::span(&probe, 1), static_cast<std::uint64_t>(world.tick()), jobs);
    REQUIRE(stats.installed > 0);
    world.step();
    world.step();
    CHECK(world.colliders(scene).empty());
    world.step();  // tick 2: installed, in key order, matching what the body says it installed
    const std::vector<ColliderInfo> installed = world.colliders(scene);
    CHECK(installed.size() == body.installed_colliders().size());
    CHECK(std::ranges::is_sorted(installed, {}, &ColliderInfo::key));
    for (int i = 0; i < 180; ++i) {
        world.step();
    }
    // On the hills (rolling down them), never through: its bottom on the ground under it.
    const math::dvec3 at = world.state(ball).position;
    double g_lo = -20.0, g_hi = 20.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (g_lo + g_hi);
        (hills->distance(at.x, mid, at.z) < 0.0 ? g_lo : g_hi) = mid;
    }
    CHECK(at.y - g_lo == doctest::Approx(0.4).epsilon(0.1));

    // A ring triangle never collides: a voxel mesh's triangles past own_triangles become adjacency.
    voxel::ColliderMesh m;
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    m.indices = {0, 2, 1, 0, 1, 2};
    m.own_triangles = 1;
    CHECK(physics_collider(m).adjacency == std::vector<std::uint8_t>{0, 1});

    // Another clock is reported, and the colliders still land at physics tick + 2.
    test::LogCapture logs;
    const voxel::ColliderProbe far{.position_m = {400.0, 3.0, -400.0}};
    const voxel::ColliderStats moved = body.update_colliders(std::span(&far, 1), 5, jobs);
    CHECK(logs.has_code("physics.collider_tick"));
    for (int i = 0; i < 3; ++i) {
        world.step();
    }
    // The far probe's chunks replace the first ones (the region moved), with none of the old keys left.
    const std::vector<ColliderInfo> now = world.colliders(scene);
    CHECK(now.size() == moved.installed);
    CHECK(std::ranges::none_of(now, [&](const ColliderInfo& c) { return c.key == installed.front().key; }));
}
