// The physics MCP tools (docs/specs/physics/spec.md): read-only inspection of the PhysicsModule's world, so an agent
// can see scenes, bodies and contacts. Registered by the engine through McpTools, next to module tools; they say so
// when the app did not import veach::PhysicsModule.

#include "physics_tools.hpp"

#include <veach/physics/module.hpp>
#include <veach/physics/physics.hpp>

#include <format>
#include <string>

namespace veach {

namespace {

using physics::PhysicsWorld;
using physics::SceneId;

nlohmann::json vec(const math::dvec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}
nlohmann::json vec(const math::vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

const char* kind_name(physics::SceneKind k) {
    switch (k) {
        case physics::SceneKind::surface: return "surface";
        case physics::SceneKind::interior: return "interior";
        case physics::SceneKind::free_flight: return "free_flight";
    }
    return "?";
}

const char* motion_name(physics::MotionType m) {
    switch (m) {
        case physics::MotionType::static_body: return "static";
        case physics::MotionType::kinematic: return "kinematic";
        case physics::MotionType::dynamic: return "dynamic";
    }
    return "?";
}

const char* ground_name(physics::GroundState g) {
    switch (g) {
        case physics::GroundState::on_ground: return "on_ground";
        case physics::GroundState::on_steep_ground: return "on_steep_ground";
        case physics::GroundState::not_supported: return "not_supported";
        case physics::GroundState::in_air: return "in_air";
    }
    return "?";
}

nlohmann::json not_imported() {
    return {{"imported", false},
            {"hint", "This app did not import veach::PhysicsModule (engine.import<veach::PhysicsModule>())."}};
}

Result<SceneId> scene_argument(const PhysicsWorld& physics, const nlohmann::json& args) {
    const auto id = args.value("scene", std::uint32_t{0});
    for (const SceneId s : physics.scenes()) {
        if (static_cast<std::uint32_t>(s) == id) {
            return s;
        }
    }
    return make_error(ErrorCode::mcp_invalid_arguments, std::format("no physics scene {}", id),
                      "physics_scenes lists the scenes and their ids.");
}

Result<McpToolOutput> physics_scenes(World& world, const nlohmann::json&) {
    const PhysicsWorld* physics = find_physics_world(world);
    if (physics == nullptr) {
        return McpToolOutput{not_imported()};
    }
    nlohmann::json list = nlohmann::json::array();
    for (const SceneId s : physics->scenes()) {
        const physics::SceneInfo info = physics->scene_info(s);
        nlohmann::json scene = {{"id", static_cast<std::uint32_t>(s)},
                                {"name", info.name},
                                {"kind", kind_name(info.kind)},
                                {"frame", std::to_string(info.frame.frame)},
                                {"frame_angular_velocity", vec(info.frame.angular_velocity)},
                                {"origin", vec(physics->origin_at(s))},
                                {"velocity", vec(info.velocity)},
                                {"bodies", info.bodies},
                                {"awake", info.awake},
                                {"characters", info.characters},
                                {"colliders", info.colliders},
                                {"pending_colliders", info.pending_colliders},
                                {"last_step_ms", info.last_step_ms}};
        if (info.lattice_origin) {
            const auto& p = *info.lattice_origin;
            scene["lattice_origin"] = {std::to_string(p[0]), std::to_string(p[1]), std::to_string(p[2])};
        }
        list.push_back(std::move(scene));
    }
    return McpToolOutput{
        nlohmann::json{{"imported", true}, {"tick", std::to_string(physics->tick())}, {"scenes", list}}};
}

Result<McpToolOutput> physics_bodies(World& world, const nlohmann::json& args) {
    const PhysicsWorld* physics = find_physics_world(world);
    if (physics == nullptr) {
        return McpToolOutput{not_imported()};
    }
    auto scene = scene_argument(*physics, args);
    if (!scene) {
        return std::unexpected(scene.error());
    }
    const auto limit = args.value("limit", std::size_t{100});
    nlohmann::json bodies = nlohmann::json::array();
    const std::vector<physics::BodyId> ids = physics->bodies(*scene);
    for (const physics::BodyId id : ids) {
        if (bodies.size() >= limit) {
            break;
        }
        const physics::BodyState s = physics->state(id);
        const physics::BodyMotion m = physics->motion(id);
        bodies.push_back(
            {{"id", static_cast<std::uint32_t>(id)},
             {"motion", motion_name(physics->settings(id).motion)},
             {"position", vec(s.position)},
             {"position_in_frame", vec(m.position)},
             {"rotation", {{"x", s.rotation.x}, {"y", s.rotation.y}, {"z", s.rotation.z}, {"w", s.rotation.w}}},
             {"linear_velocity", vec(s.linear_velocity)},
             {"angular_velocity", vec(s.angular_velocity)},
             {"mass", physics->mass(id)},
             {"awake", s.awake}});
    }
    nlohmann::json characters = nlohmann::json::array();
    for (const physics::CharacterId id : physics->characters(*scene)) {
        const physics::CharacterState c = physics->character(id);
        characters.push_back({{"id", static_cast<std::uint32_t>(id)},
                              {"position", vec(c.position)},
                              {"linear_velocity", vec(c.linear_velocity)},
                              {"ground", ground_name(c.ground)},
                              {"supported", c.supported}});
    }
    nlohmann::json colliders = nlohmann::json::array();
    for (const physics::ColliderInfo& c : physics->colliders(*scene)) {
        if (colliders.size() >= limit) {
            break;
        }
        colliders.push_back({{"key", {{"level", c.key.level}, {"x", c.key.x}, {"y", c.key.y}, {"z", c.key.z}}},
                             {"position", vec(c.position)},
                             {"triangles", c.triangles},
                             {"shape_hash", std::format("{:016x}", c.shape_hash)}});
    }
    return McpToolOutput{nlohmann::json{{"scene", static_cast<std::uint32_t>(*scene)},
                                        {"total_bodies", ids.size()},
                                        {"bodies", bodies},
                                        {"characters", characters},
                                        {"colliders", colliders}}};
}

Result<McpToolOutput> physics_contacts(World& world, const nlohmann::json& args) {
    const PhysicsWorld* physics = find_physics_world(world);
    if (physics == nullptr) {
        return McpToolOutput{not_imported()};
    }
    const auto limit = args.value("limit", std::size_t{100});
    nlohmann::json list = nlohmann::json::array();
    for (const physics::Contact& c : physics->contacts()) {
        if (list.size() >= limit) {
            break;
        }
        nlohmann::json contact = {{"kind", c.kind == physics::Contact::Kind::added ? "added" : "removed"},
                                  {"scene", static_cast<std::uint32_t>(c.scene)},
                                  {"a", static_cast<std::uint32_t>(c.a)},
                                  {"b", static_cast<std::uint32_t>(c.b)},
                                  {"sub_shape_a", c.sub_shape_a},
                                  {"sub_shape_b", c.sub_shape_b}};
        if (c.kind == physics::Contact::Kind::added) {
            contact["point"] = vec(c.point);
            contact["normal"] = vec(c.normal);
            contact["depth"] = c.depth;
        }
        list.push_back(std::move(contact));
    }
    return McpToolOutput{nlohmann::json{
        {"tick", std::to_string(physics->tick())}, {"total", physics->contacts().size()}, {"contacts", list}}};
}

nlohmann::json limit_schema(bool scene) {
    nlohmann::json properties = {{"limit", {{"type", "integer"}, {"minimum", 1}}}};
    if (scene) {
        properties["scene"] = {{"type", "integer"}, {"minimum", 0}};
    }
    return {{"type", "object"}, {"properties", properties}};
}

}  // namespace

void register_physics_tools(McpTools& tools) {
    static_cast<void>(tools.add(
        {.name = "physics_scenes",
         .description = "The physics scenes of veach::PhysicsModule (ADR 0058: one per reference frame): id, name, "
                        "kind (surface | interior | free_flight), frame entity, origin and velocity in that frame "
                        "(m, m/s), lattice origin, counts of bodies, awake bodies, characters, installed and pending "
                        "terrain colliders, and the last step's ms. Read-only.",
         .input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}},
         .handler = physics_scenes}));
    static_cast<void>(tools.add(
        {.name = "physics_bodies",
         .description = "The bodies, characters and terrain colliders of one physics scene (default 0): position in "
                        "scene coordinates and in the scene's frame, rotation, velocities, mass, awake; characters' "
                        "ground state; colliders' chunk keys. Read-only.",
         .input_schema = limit_schema(true),
         .handler = physics_bodies}));
    static_cast<void>(tools.add(
        {.name = "physics_contacts",
         .description = "The contact events of the last physics step, sorted by (scene, body, body, sub-shape): "
                        "added contacts with point, normal and depth, and removed ones. Read-only.",
         .input_schema = limit_schema(false),
         .handler = physics_contacts}));
}

}  // namespace veach
