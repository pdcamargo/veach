#include "sp13_spike.hpp"

#include "sp13/sp13_sim.hpp"

#include <veach/core/env.hpp>

#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

namespace lab {
namespace {

namespace math = veach::math;
namespace sp13 = veach::sp13;

/// The pad's tangent frame: x east, y up, z south (−Z is north), meters from the pad.
struct Local {
    sp13::Planet p;
    [[nodiscard]] math::dvec3 dir(math::dvec3 v) const {
        return {math::dot(v, p.site_east), math::dot(v, p.site_up), -math::dot(v, p.site_north)};
    }
    [[nodiscard]] math::vec3 point(math::dvec3 b) const { return math::vec3(dir(b - p.site)); }
    [[nodiscard]] math::quat upright(math::dvec3 b) const {
        const math::vec3 up(dir(math::normalize(b)));
        const math::vec3 y(0.0f, 1.0f, 0.0f);
        const math::vec3 c = math::cross(y, up);
        return math::normalize(math::quat(1.0f + math::dot(y, up), c.x, c.y, c.z));
    }
};

veach::render::MeshData box_mesh(veach::AssetId material) {
    veach::render::MeshData data;
    data.materials = {material};
    const math::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const math::vec3& n : normals) {
        const math::vec3 u = std::abs(n.y) > 0.5f ? math::vec3(1, 0, 0) : math::vec3(0, 1, 0);
        const math::vec3 v = math::cross(n, u);
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners) {
            const math::vec3 pos = (n + u * c[0] + v * c[1]) * 0.5f;
            veach::MeshVertex vertex;
            vertex.position = {pos.x, pos.y, pos.z};
            vertex.normal = {n.x, n.y, n.z};
            vertex.tangent = {u.x, u.y, u.z, 1.0f};
            vertex.uv = {c[0] * 0.5f + 0.5f, c[1] * 0.5f + 0.5f};
            data.vertices.push_back(vertex);
        }
        // Counter-clockwise seen from outside: u × v = n.
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return data;
}

/// The sphere around the pad, ±half m, `cell` m cells, normals radial.
veach::render::MeshData terrain_mesh(const Local& l, double half, double cell, veach::AssetId material) {
    const auto n = static_cast<std::uint32_t>(2.0 * half / cell);
    veach::render::MeshData data;
    data.materials = {material};
    for (std::uint32_t j = 0; j <= n; ++j) {
        for (std::uint32_t i = 0; i <= n; ++i) {
            const double e = -half + cell * i;
            const double s = -half + cell * j;
            const math::dvec3 b = math::normalize(l.p.site + l.p.site_east * e + l.p.site_north * s) * l.p.radius;
            const math::vec3 pos = l.point(b);
            const math::vec3 normal(l.dir(math::normalize(b)));
            veach::MeshVertex vertex;
            vertex.position = {pos.x, pos.y, pos.z};
            vertex.normal = {normal.x, normal.y, normal.z};
            vertex.uv = {static_cast<float>(e / 8.0), static_cast<float>(s / 8.0)};
            data.vertices.push_back(vertex);
        }
    }
    for (std::uint32_t j = 0; j < n; ++j) {
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t a = j * (n + 1) + i;  // j grows north, which is −z: east × north = up
            data.indices.insert(data.indices.end(), {a, a + 1, a + n + 2, a, a + n + 2, a + n + 1});
        }
    }
    return data;
}

}  // namespace

void register_sp13_spike(veach::World& world) {
    const std::optional<std::string> scale_text = veach::env("VEACH_SP13");
    if (!scale_text) {
        return;
    }
    const double scale = std::strtod(scale_text->c_str(), nullptr);
    const std::string at = veach::env("VEACH_SP13_AT").value_or("hover");

    sp13::Sim sim(sp13::Config{.scale = scale});
    sp13::Autopilot pilot(sim.planet(), false);
    sp13::Controls c;
    int after = 0;
    const sp13::Autopilot::Phase wanted =
        at == "landed" ? sp13::Autopilot::Phase::landed : sp13::Autopilot::Phase::hover;
    while (sim.tick() < 60 * 600 && after < 60) {
        sim.step(c);
        c = pilot.next(sim.craft());
        after += pilot.phase() == wanted ? 1 : 0;
    }
    const sp13::CraftView v = sim.craft();
    veach::log::info("sp13",
                     "s = {}: tick {}, craft {:.2f} m above the ground, {:.2f} m from the pad, walker {:.1f} m away",
                     scale, sim.tick(), v.altitude, std::sqrt(v.offset_e * v.offset_e + v.offset_n * v.offset_n),
                     math::length(sim.position_body("walker") - sim.planet().site));

    auto& meshes = veach::services(world).get<veach::render::MeshStore>();
    const auto material = [&](const char* name, math::vec4 color, float roughness) {
        return meshes.add_material({.name = name, .base_color = color, .metallic = 0.0f, .roughness = roughness});
    };
    const Local l{sim.planet()};
    const auto ground = meshes.add(terrain_mesh(l, 1500.0, 10.0, material("ground", {0.30f, 0.27f, 0.22f, 1}, 0.9f)));
    const auto box = [&](math::vec4 color) {
        return meshes.add(box_mesh(material("box", color, 0.6f)));
    };
    const auto grey = box({0.55f, 0.56f, 0.58f, 1});
    const auto brown = box({0.45f, 0.28f, 0.12f, 1});
    const auto white = box({0.85f, 0.85f, 0.82f, 1});
    const auto orange = box({0.9f, 0.4f, 0.08f, 1});
    const auto pad = box({0.08f, 0.08f, 0.09f, 1});
    if (!ground || !grey || !brown || !white || !orange || !pad) {
        veach::log::error("sp13", "could not build the meshes");
        return;
    }
    world.entity("sp13_ground").set<veach::Transform>({}).set(veach::RuntimeMeshInstance{*ground});
    const auto place = [&](const char* name, veach::AssetId mesh, math::dvec3 b, math::vec3 size, float lift) {
        const math::quat q = l.upright(b);
        world.entity(name)
            .set<veach::Transform>({.translation = math::dvec3(l.point(b) + q * math::vec3(0.0f, lift, 0.0f)),
                                    .rotation = q,
                                    .scale = size})
            .set(veach::RuntimeMeshInstance{mesh});
    };
    place("sp13_pad", *pad, sim.planet().site, {14.0f, 0.1f, 14.0f}, 0.0f);
    place("sp13_base", *grey, sim.position_body("base"), {12.0f, 5.0f, 12.0f}, 0.0f);
    place("sp13_crate_sleepy", *brown, sim.position_body("crate_sleepy"), {1.0f, 1.0f, 1.0f}, 0.0f);
    place("sp13_crate_awake", *brown, sim.position_body("crate_awake"), {1.0f, 1.0f, 1.0f}, 0.0f);
    place("sp13_idler", *orange, sim.position_body("idler"), {0.6f, 1.8f, 0.6f}, 0.9f);
    place("sp13_walker", *orange, sim.position_body("walker"), {0.6f, 1.8f, 0.6f}, 0.9f);
    place("sp13_craft", *white, sim.craft_position_body(), {4.0f, 2.0f, 4.0f}, 0.0f);

    // The camera south-west of the pad, looking at a point between the craft and the base.
    const math::vec3 eye = at == "landed" ? math::vec3(-28.0f, 6.0f, 34.0f) : math::vec3(-30.0f, 12.0f, 55.0f);
    const math::vec3 target = at == "landed" ? math::vec3(14.0f, 1.5f, 0.0f) : math::vec3(12.0f, 30.0f, 0.0f);
    world.entity("sp13_camera")
        .set<veach::Transform>({.translation = math::dvec3(eye),
                                .rotation = math::quatLookAtRH(math::normalize(target - eye), math::vec3(0, 1, 0))})
        .set<veach::Camera>({.fov_y = 0.9f, .near_plane = 0.1f, .ev100 = 14.5f});
    world.entity("sp13_sky").set<veach::Sky>({.time_of_day = 16.0f, .latitude = 30.0f, .day_of_year = 172.0f});
    world.system<veach::Window>("Sp13Size").kind<veach::phase::Startup>().each([](veach::Window& window) {
        window.width = 960;
        window.height = 540;
    });
}

}  // namespace lab
