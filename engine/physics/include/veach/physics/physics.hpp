#pragma once

// veach::physics: rigid bodies, characters and terrain colliders, one physics scene per reference frame
// (docs/specs/physics/spec.md, docs/guides/physics.md; ADR 0058, amended by 0075, 0077, 0080 and 0083).
//
// Jolt Physics, built double precision and cross-platform deterministic, is private to this area: no type here is a
// Jolt type. A PhysicsWorld owns scenes; each scene is a Jolt PhysicsSystem with its own temp allocator, near its own
// origin in a parent reference frame (a body-fixed surface region, a ship interior, a free-flight scene that follows a
// fast craft). Positions are double, in scene coordinates (relative to the scene origin, in the parent frame's axes)
// or in the parent frame; Jolt stores rotations and velocities in float32, and the state accessors return them as
// stored, so a hash of them is a hash of the simulation.
//
//   veach::physics::PhysicsWorld physics({.jobs = &jobs});
//   const auto scene = physics.create_scene({.name = "pad", .kind = SceneKind::surface});
//   physics.set_gravity([](const GravityQuery& q) { return q.position * (-gm / pow(length(q.position), 3)); });
//   const auto ball = physics.add_body(scene, {.shape = Shape::sphere(0.4f), .position = {0.0, 2.0, 0.0}});
//   physics.step();                                      // one fixed step of 1/60 s
//
// Determinism is replay tier (ADR 0043): the same inputs in the same order give the same bits on every thread count,
// and the build flags make them the same on macOS arm64 and Windows x64. Everything that reaches Jolt is ordered:
// bodies by id, scenes by id, colliders by chunk key, contact events sorted.

#include <veach/core/error.hpp>
#include <veach/core/math.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace veach {
class JobSystem;
}

namespace veach::physics {

/// Initializes Jolt's process-wide state once: allocator hooks, the type factory and registry, trace and assert
/// hooks (ADR 0075 decision 5). Thread-safe and idempotent; PhysicsWorld and PhysicsModule call it.
void initialize();

/// The fixed step physics advances by, seconds (veach::fixed_delta_seconds as Jolt's float).
inline constexpr float step_seconds = 1.0f / 60.0f;
/// K: a collider requested at tick t is installed before the step of tick t + K, on every machine (ADR 0077).
inline constexpr std::int64_t collider_install_delay = 2;

enum class SceneId : std::uint32_t {};
enum class BodyId : std::uint32_t {};
enum class CharacterId : std::uint32_t {};

// ---- Shapes --------------------------------------------------------------------------------------------------

/// An immutable collision shape, shared between bodies and scenes (reference counted). Empty when default-built.
class Shape {
public:
    Shape() = default;

    /// A box of half extents `half` (Jolt's default convex radius, 5 cm).
    [[nodiscard]] static Shape box(const math::vec3& half);
    [[nodiscard]] static Shape sphere(float radius);
    /// A capsule along +Y: a cylinder of half height `half_height` capped by hemispheres of `radius`.
    [[nodiscard]] static Shape capsule(float half_height, float radius);
    /// `inner` moved by `translation` and turned by `rotation` (a character's capsule above its feet).
    [[nodiscard]] static Result<Shape> offset(const Shape& inner, const math::vec3& translation,
                                              const math::quat& rotation = math::quat(1.0f, 0.0f, 0.0f, 0.0f));
    /// A static triangle mesh; triangles wind counter-clockwise seen from the side that collides.
    [[nodiscard]] static Result<Shape> mesh(std::span<const math::vec3> vertices,
                                            std::span<const std::array<std::uint32_t, 3>> triangles);
    /// A height field of samples × samples heights (row-major, z then x), at offset + (x, height, z) * scale.
    [[nodiscard]] static Result<Shape> height_field(std::span<const float> heights, std::uint32_t samples,
                                                    const math::vec3& offset, const math::vec3& scale);

    [[nodiscard]] explicit operator bool() const { return impl_ != nullptr; }
    /// FNV-1a of the shape's binary state (a cooked mesh's tree included): equal shapes hash equal.
    [[nodiscard]] std::uint64_t hash() const;
    [[nodiscard]] std::uint32_t triangle_count() const;

    struct Impl;
    explicit Shape(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
    [[nodiscard]] const Impl* impl() const { return impl_.get(); }

private:
    std::shared_ptr<const Impl> impl_;
};

// ---- Scenes --------------------------------------------------------------------------------------------------

enum class SceneKind : std::uint8_t {
    surface,      ///< body-fixed region of terrain, bases and slow occupants; no fictitious forces (ADR 0058)
    interior,     ///< a ship's inside, in the ship's frame; its acceleration is not applied (inertial dampers)
    free_flight,  ///< follows a fast craft: its origin moves at `velocity` (ADR 0080 decision 2)
};

/// The integer lattice terrain colliders are placed on (ADR 0074, 0077 decision 6): one unit is `unit_meters`,
/// mesh vertices are fixed point, units × 2^fixed_bits. voxel's LOD0 lattice is the default.
struct Lattice {
    double unit_meters = 0.5;
    int fixed_bits = 16;
};
using LatticePoint = std::array<std::int64_t, 3>;

/// The frame a scene lives in. `frame` is an opaque tag for its owner (the ReferenceFrame entity, VCH-111); physics
/// never reads it. `angular_velocity` is the frame's spin in its own axes, rad/s: zero for an inertial frame.
struct SceneFrame {
    std::uint64_t frame = 0;
    math::dvec3 angular_velocity{0.0};
};

struct SceneSettings {
    std::string name;
    SceneKind kind = SceneKind::surface;
    SceneFrame frame;
    math::dvec3 origin{0.0};    ///< in the parent frame, m (ignored when lattice_origin is set)
    math::dvec3 velocity{0.0};  ///< of the origin, m/s: free-flight scenes only
    /// A surface scene that holds terrain colliders sits on a lattice point with the frame's axes, so collider
    /// vertices are the canonical mesh's lattice positions exactly; origin = lattice_origin * unit_meters.
    std::optional<LatticePoint> lattice_origin;
    Lattice lattice;
    /// Coriolis and centrifugal accelerations for bodies of a free-flight scene in a rotating frame (ADR 0080
    /// decision 1). Surface and interior scenes never get them. Off only for control runs.
    bool fictitious_forces = true;
    std::uint32_t max_bodies = 1024;
    std::uint32_t max_body_pairs = 4096;
    std::uint32_t max_contact_constraints = 4096;
    std::size_t temp_allocator_bytes = std::size_t{16} << 20;
    float collider_friction = 0.2f;  ///< terrain colliders' friction and restitution
    float collider_restitution = 0.0f;
};

struct SceneInfo {
    SceneId id{};
    std::string name;
    SceneKind kind = SceneKind::surface;
    SceneFrame frame;
    math::dvec3 origin{0.0};  ///< at origin_tick
    math::dvec3 velocity{0.0};
    std::int64_t origin_tick = 0;
    std::optional<LatticePoint> lattice_origin;
    std::size_t bodies = 0, awake = 0, characters = 0, colliders = 0, pending_colliders = 0;
    double last_step_ms = 0.0;  ///< wall time of the last step (a statistic, never fed back)
};

// ---- Bodies --------------------------------------------------------------------------------------------------

enum class MotionType : std::uint8_t { static_body, kinematic, dynamic };

/// A rigid body. Position, rotation and velocities are in scene coordinates (velocities relative to the scene's
/// moving origin); defaults are Jolt's.
struct BodySettings {
    Shape shape;
    MotionType motion = MotionType::dynamic;
    math::dvec3 position{0.0};
    math::dquat rotation{1.0, 0.0, 0.0, 0.0};  ///< rounded to float32 and normalized
    math::dvec3 linear_velocity{0.0};
    math::dvec3 angular_velocity{0.0};
    float friction = 0.2f;
    float restitution = 0.0f;
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    float max_linear_velocity = 500.0f;  ///< m/s
    std::optional<double> mass;          ///< kg, with the inertia computed from the shape; else from its density
    bool allow_sleeping = true;
    bool lock_rotation = false;  ///< translation only (a craft that steers its own attitude)
    bool gravity = true;         ///< dynamic: the gravity hook applies to it
};

/// A body's state as Jolt stores it: position double, rotation and velocities float32.
struct BodyState {
    math::dvec3 position{0.0};  ///< scene coordinates
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    math::vec3 linear_velocity{0.0f};  ///< relative to the scene origin's velocity
    math::vec3 angular_velocity{0.0f};
    bool awake = false;
};

/// Pose and velocity in the scene's parent frame, double.
struct BodyMotion {
    math::dvec3 position{0.0};
    math::dvec3 velocity{0.0};
    math::dquat rotation{1.0, 0.0, 0.0, 0.0};
    math::dvec3 angular_velocity{0.0};
};

struct TransferOptions {
    /// The destination's parent frame changes first (a free-flight scene crossing a handoff altitude); the
    /// destination must hold no other body.
    std::optional<SceneFrame> frame;
    /// Re-center the destination on the arriving body: its origin and velocity become the body's (free flight).
    bool recenter = false;
};

// ---- Gravity -------------------------------------------------------------------------------------------------

/// What the gravity hook is asked: an awake dynamic body or a character, in its scene's parent frame.
struct GravityQuery {
    SceneId scene{};
    std::uint64_t frame = 0;  ///< the scene's SceneFrame::frame
    math::dvec3 position{0.0};
    math::dvec3 velocity{0.0};
};
/// Returns the gravitational acceleration at the query, m/s², in the parent frame's axes. Supplied by gameplay or
/// the toolbox (the gravity registry, ADR 0059): physics never reads G or a body's mass. Called on the main thread.
using GravityFn = std::function<math::dvec3(const GravityQuery&)>;

// ---- Characters ----------------------------------------------------------------------------------------------

/// A kinematic character controller with an arbitrary up (Jolt's CharacterVirtual).
struct CharacterSettings {
    Shape shape;                            ///< in the character's space, feet at the origin, its +Y up
    float supporting_plane = -0.3f;         ///< contacts below this height (+Y) support the character
    float max_slope = 0.7853981633974483f;  ///< rad (45°)
    float step_down = 0.5f;                 ///< stick to the floor this far down, m
    float step_up = 0.4f;                   ///< climb steps this high, m
    bool enhanced_internal_edge_removal = false;
    math::dvec3 position{0.0};  ///< scene coordinates
    math::dquat rotation{1.0, 0.0, 0.0, 0.0};
    math::dvec3 up{0.0, 1.0, 0.0};
};

/// Gameplay's wish for the next step: up (−ĝ, filtered as the game likes), the character's rotation and how it
/// walks. The step keeps the velocity along up (or the ground's velocity when standing), adds g·dt from the gravity
/// hook and then `move`, and moves the character with sliding, stairs and floor sticking.
struct CharacterInput {
    math::dvec3 up{0.0, 1.0, 0.0};
    math::dquat rotation{1.0, 0.0, 0.0, 0.0};
    std::optional<math::dvec3> move;  ///< walking velocity, m/s; empty: standing (nothing added)
};

enum class GroundState : std::uint8_t { on_ground, on_steep_ground, not_supported, in_air };

struct CharacterState {
    math::dvec3 position{0.0};  ///< scene coordinates
    math::vec3 linear_velocity{0.0f};
    math::vec3 ground_velocity{0.0f};
    GroundState ground = GroundState::in_air;
    bool supported = false;
};

// ---- Contacts and queries ------------------------------------------------------------------------------------

/// A contact that began (or ended) in the last step. Sorted by (a, b, sub_shape_a, sub_shape_b), with a < b; the
/// normal points from a to b.
struct Contact {
    enum class Kind : std::uint8_t { added, removed };
    Kind kind = Kind::added;
    SceneId scene{};
    BodyId a{}, b{};
    std::uint32_t sub_shape_a = 0, sub_shape_b = 0;
    math::dvec3 point{0.0};  ///< scene coordinates (added only)
    math::vec3 normal{0.0f};
    float depth = 0.0f;
};

struct RayHit {
    double fraction = 0.0;  ///< along the ray, 0..1
    BodyId body{};
};

/// What a ray cast sees.
enum class RayFilter : std::uint8_t {
    all,          ///< every body and collider
    static_only,  ///< static bodies and terrain colliders: what stays put (not craft, props or characters)
};

/// How open a point is to the sky (sky_visibility).
struct SkyOpenness {
    float visibility = 1.0f;  ///< 0..1: the cosine-weighted fraction of the sky the static world leaves open
    float reach = 0.0f;       ///< meters: how far the blocking surroundings extend (0 when nothing blocks)
};

// ---- Terrain colliders ---------------------------------------------------------------------------------------

/// A chunk of a body-local octree (voxel's ChunkKey, field for field and in the same order): the install order.
struct ChunkKey {
    std::uint8_t level = 0;
    std::int32_t x = 0, y = 0, z = 0;
    auto operator<=>(const ChunkKey&) const = default;
};

/// A terrain collider's mesh, as the voxel bridge hands it over (VCH-114): canonical, in lattice coordinates.
/// Vertices are fixed point (lattice units × 2^fixed_bits) in the lattice's frame, the same integers the mesher
/// hashes; physics places them relative to `origin`, exactly. `adjacency` marks triangles (one byte per triangle,
/// empty: none) that come from the neighbors' borders: they inform Jolt's internal-edge handling and never collide
/// (ADR 0083). A mesh without triangles removes the chunk's collider.
struct ColliderMesh {
    ChunkKey key;
    LatticePoint origin{};     ///< the chunk's minimum corner, lattice units
    std::int64_t extent = 32;  ///< the chunk's edge, lattice units (the box whose bodies wake on a swap)
    std::vector<std::array<std::int64_t, 3>> vertices;
    std::vector<std::uint32_t> indices;  ///< three per triangle, counter-clockwise seen from outside the solid
    std::vector<std::uint8_t> adjacency;
};

/// One chunk's triangles for with_adjacency_ring: fixed-point vertices and indices, as in ColliderMesh.
struct ChunkTriangles {
    std::span<const std::array<std::int64_t, 3>> vertices;
    std::span<const std::uint32_t> indices;
};

/// The collider of the chunk at `origin` (edge `extent` units) cooked with a ring of its neighbors' border triangles
/// (ADR 0083): `neighborhood` holds the 27 chunks around it, z then y then x from −1 to +1 (index 13 is the chunk
/// itself; null spans for absent neighbors). Its own triangles and every neighbor triangle with a vertex on its box,
/// in that order, vertices welded by lattice position in first-use order.
[[nodiscard]] ColliderMesh with_adjacency_ring(const ChunkKey& key, const LatticePoint& origin, std::int64_t extent,
                                               std::span<const ChunkTriangles, 27> neighborhood, int fixed_bits = 16);

/// A collider cooked from a ColliderMesh, ready to install; shared and immutable.
struct CookedCollider {
    ChunkKey key;
    LatticePoint origin{};
    std::int64_t extent = 32;
    Shape shape;  ///< empty: remove the chunk's collider
    std::uint32_t triangles = 0;
};

/// Cooks a collider (a Jolt MeshShape) from its mesh. Thread-safe: run it on job threads, one chunk per call.
[[nodiscard]] Result<CookedCollider> cook_collider(const ColliderMesh& mesh, const Lattice& lattice = {});

struct ColliderInfo {
    ChunkKey key;
    std::uint32_t slot = 0;  ///< the body's Jolt index and sequence number: it records the install order
    math::dvec3 position{0.0};
    std::uint64_t shape_hash = 0;
    std::uint32_t triangles = 0;
};

// ---- The world -----------------------------------------------------------------------------------------------

struct WorldSettings {
    JobSystem* jobs = nullptr;  ///< the engine's workers; null: everything on the calling thread
    /// The most threads one step spreads its work over (Jolt sizes its job batches by it); results never depend on
    /// it. Measured on the M5 Max: 4 is fastest for 200 to 2,000 bodies, and spreading over all 17 workers costs more
    /// than it gains (docs/guides/physics.md).
    std::uint32_t max_concurrency = 4;
};

class PhysicsWorld {
public:
    explicit PhysicsWorld(const WorldSettings& settings = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    /// The number of steps taken.
    [[nodiscard]] std::int64_t tick() const;

    /// One fixed step from tick t to t + 1, in this order: colliders due at t (chunk-key order, waking the bodies
    /// over swapped ones), the gravity hook for every awake dynamic body (body id order, plus fictitious forces in
    /// rotating free flight), characters (id order), then each scene with bodies (id order); then contact events.
    void step();

    // Scenes.
    [[nodiscard]] Result<SceneId> create_scene(const SceneSettings& settings);
    void destroy_scene(SceneId scene);
    [[nodiscard]] std::vector<SceneId> scenes() const;
    [[nodiscard]] SceneInfo scene_info(SceneId scene) const;
    /// The scene origin at the current tick, in the parent frame: origin + velocity · (tick − origin_tick) / 60.
    [[nodiscard]] math::dvec3 origin_at(SceneId scene) const;
    /// Replaces the scene's frame tag and spin (a frame whose spin rate changes); bodies are untouched.
    void set_frame(SceneId scene, const SceneFrame& frame);
    /// Re-anchors a scene at a new origin (parent frame) at this tick boundary: every body and character shifts by
    /// the difference in double; velocities are unchanged. Clears the scene's lattice origin.
    void set_origin(SceneId scene, const math::dvec3& origin);
    /// Free flight: a new origin and origin velocity at this tick; every body keeps its parent-frame state.
    void set_origin(SceneId scene, const math::dvec3& origin, const math::dvec3& velocity);
    /// Re-anchors on a lattice point (terrain scenes stay exact, ADR 0077 decision 6).
    void set_origin(SceneId scene, const LatticePoint& lattice_origin);
    /// Free flight: the scene's origin and velocity become `body`'s at this tick, so its offsets are exactly zero;
    /// other bodies keep their parent-frame state.
    void recenter(SceneId scene, BodyId body);
    /// Rebuilds the broad phase tree; call after adding many bodies at once. Part of the deterministic input.
    void optimize(SceneId scene);
    /// Jolt's own SaveState of the scene, FNV-1a: bodies, contacts, warm-start impulses.
    [[nodiscard]] std::uint64_t state_hash(SceneId scene) const;

    // Bodies.
    [[nodiscard]] Result<BodyId> add_body(SceneId scene, const BodySettings& settings);
    void remove_body(BodyId body);
    /// Moves `body` into `to` at this tick boundary, with pose and velocity given in to's parent frame (converted by
    /// the caller in double: frames are VCH-111's). The body keeps its id and settings.
    [[nodiscard]] Result<> transfer(BodyId body, SceneId to, const BodyMotion& motion,
                                    const TransferOptions& options = {});
    [[nodiscard]] bool contains(BodyId body) const;
    [[nodiscard]] SceneId scene_of(BodyId body) const;
    [[nodiscard]] std::vector<BodyId> bodies(SceneId scene) const;
    [[nodiscard]] BodyState state(BodyId body) const;
    [[nodiscard]] BodyMotion motion(BodyId body) const;
    [[nodiscard]] const BodySettings& settings(BodyId body) const;
    [[nodiscard]] double mass(BodyId body) const;
    /// Jolt's float32 inverse mass, exactly; 0 for static and kinematic bodies.
    [[nodiscard]] double inverse_mass(BodyId body) const;
    [[nodiscard]] bool awake(BodyId body) const;
    /// Added to the body's force for the next step, rounded to float32; never wakes it.
    void add_force(BodyId body, const math::dvec3& force);
    void add_impulse(BodyId body, const math::dvec3& impulse);
    void set_rotation(BodyId body, const math::dquat& rotation);
    void set_linear_velocity(BodyId body, const math::dvec3& velocity);  ///< scene coordinates
    void wake(BodyId body);

    // Characters.
    [[nodiscard]] Result<CharacterId> add_character(SceneId scene, const CharacterSettings& settings);
    void remove_character(CharacterId character);
    [[nodiscard]] std::vector<CharacterId> characters(SceneId scene) const;
    void set_input(CharacterId character, const CharacterInput& input);
    [[nodiscard]] CharacterState character(CharacterId character) const;
    [[nodiscard]] SceneId scene_of(CharacterId character) const;

    /// The gravity hook (ADR 0058, 0059). Without one, nothing falls.
    void set_gravity(GravityFn gravity);

    // Terrain colliders.
    /// Requests colliders at this tick: cooked on the job system now, installed before step tick() + K in chunk-key
    /// order (a later request for the same chunk in the same tick wins). If a cook is not done by then, the step
    /// waits: a slow machine stalls, it never simulates differently (ADR 0058). Fails with physics.not_on_lattice.
    [[nodiscard]] Result<> request_colliders(SceneId scene, std::vector<ColliderMesh> meshes);
    /// Installs cooked colliders now, in the order given (loading a region before its first step).
    [[nodiscard]] Result<> install_colliders(SceneId scene, std::span<const CookedCollider> colliders);
    /// Installed terrain colliders in chunk-key order.
    [[nodiscard]] std::vector<ColliderInfo> colliders(SceneId scene) const;

    // Queries and events.
    /// The first hit along `direction` (its length is the ray's) from `from`, scene coordinates.
    [[nodiscard]] std::optional<RayHit> cast_ray(SceneId scene, const math::dvec3& from, const math::vec3& direction,
                                                 RayFilter filter = RayFilter::all) const;
    /// The last step's contact events, sorted.
    [[nodiscard]] std::span<const Contact> contacts() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

/// How open `from` (scene coordinates) is to the sky around `up` (unit), for the renderer's occluded aerial
/// perspective and ambient light (ADR 0082 rule 5, ADR 0091; render::SkyVisibility): `rays` fixed cosine-weighted
/// directions over the upper hemisphere, each cast `max_distance` meters against static bodies and terrain colliders.
/// The reach is where three quarters of the blocked rays have hit. Visual only: never feed it to the simulation.
[[nodiscard]] SkyOpenness sky_visibility(const PhysicsWorld& physics, SceneId scene, const math::dvec3& from,
                                         const math::vec3& up, float max_distance = 64.0f, std::uint32_t rays = 32);

}  // namespace veach::physics
