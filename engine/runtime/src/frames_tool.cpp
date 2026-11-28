// The `frames` MCP tool (docs/specs/frames/spec.md, ADR 0046): the reference-frame tree, so an agent can see where
// things are without system coordinates. Registered by the engine through McpTools, next to module tools.

#include "frames_tool.hpp"

#include <veach/ecs/components.hpp>
#include <veach/ecs/frames.hpp>
#include <veach/ecs/json.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace veach {

namespace {

std::string path_of(const World& world, ecs::entity_t e) {
    return e != 0 ? std::string(world.entity(e).path("::", "").c_str()) : std::string();
}

Result<McpToolOutput> frames(World& world, const nlohmann::json&) {
    update_frames(world);  // the tree as it is now, even between a write and the next frame pass
    std::map<ecs::entity_t, std::int64_t> members;
    world.each([&](const FrameTransform& pose) { ++members[pose.frame]; });

    std::vector<std::tuple<std::uint32_t, std::string, ecs::entity_t>> order;
    world.each([&](Entity e, const ReferenceFrame&, const FrameLink& link) {
        order.emplace_back(link.depth, path_of(world, e.id()), e.id());
    });
    std::ranges::sort(order);
    nlohmann::json list = nlohmann::json::array();
    for (const auto& [depth, path, id] : order) {
        const Entity e = world.entity(id);
        const FrameLink& link = e.get<FrameLink>();
        nlohmann::json frame = to_json(world, world.id<ReferenceFrame>(), &e.get<ReferenceFrame>());
        frame["path"] = path;
        frame["parent"] = link.parent != 0 ? nlohmann::json(path_of(world, link.parent)) : nlohmann::json(nullptr);
        frame["depth"] = depth;
        frame["entities"] = members.contains(id) ? members[id] : 0;
        if (e.has<FrameSpin>()) {
            frame["spin"] = to_json(world, world.id<FrameSpin>(), &e.get<FrameSpin>());
        }
        list.push_back(std::move(frame));
    }
    return McpToolOutput{nlohmann::json{{"root_entities", members.contains(0) ? members[0] : 0}, {"frames", list}}};
}

}  // namespace

void register_frames_tool(McpTools& tools) {
    static_cast<void>(tools.add(
        {.name = "frames",
         .description = "The reference-frame tree (ADR 0045): every frame entity with its parent frame, depth, kind "
                        "(inertial | rotating), up (axis_y | radial), pose relative to its parent frame (position "
                        "[x, y, z] m, orientation {x, y, z, w}, linear_velocity m/s, angular_velocity rad/s, in the "
                        "parent's axes) and how many entities live in it. Positions anywhere else are relative to "
                        "the entity's frame (get_component Transform / FrameTransform). Read-only.",
         .input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}},
         .handler = frames}));
}

}  // namespace veach
