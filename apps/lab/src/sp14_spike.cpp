#include "sp14_spike.hpp"

#include "sp10_spike.hpp"

#include <veach/core/env.hpp>
#include <veach/render/atmosphere.hpp>
#include <veach/render/renderer.hpp>
#include <veach/space/atmosphere.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lab {

namespace {

namespace math = veach::math;
namespace render = veach::render;
namespace vs = veach::space;

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;
constexpr double sun_lux = 128000.0;  ///< 1 AU, above any air
constexpr double eye_height = 1.7;    ///< m: the ground is at y = -eye_height in the exposure scene

double number(const char* name, double fallback) {
    const std::optional<std::string> text = veach::env(name);
    return text ? std::strtod(text->c_str(), nullptr) : fallback;
}

/// Forward for a yaw (from -Z toward +X) and pitch (up), +Y up.
math::dvec3 forward_of(double yaw, double pitch) {
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

/// The camera's rotation looking along `f` with +Y up (or -Z when looking straight up or down).
math::dmat3 rotation_toward(math::dvec3 f) {
    f = math::normalize(f);
    const math::dvec3 reference = std::abs(f.y) > 0.999 ? math::dvec3(0.0, 0.0, -1.0) : math::dvec3(0.0, 1.0, 0.0);
    const math::dvec3 right = math::normalize(math::cross(f, reference));
    const math::dvec3 up = math::cross(right, f);
    return {right, up, -f};
}

/// `base` turned by `angle` toward `toward` (both unit vectors).
math::dvec3 turned(math::dvec3 base, math::dvec3 toward, double angle) {
    const math::dvec3 axis = math::normalize(math::cross(base, toward));
    return base * std::cos(angle) + math::cross(axis, base) * std::sin(angle);
}

// ---- meshes ---------------------------------------------------------------------------------------------------

struct Builder {
    render::MeshData data;

    std::uint32_t vertex(math::dvec3 p, math::dvec3 n) {
        veach::MeshVertex v;
        v.position = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
        v.normal = {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
        const math::dvec3 t =
            math::normalize(math::cross(std::abs(n.y) < 0.9 ? math::dvec3(0, 1, 0) : math::dvec3(1, 0, 0), n));
        v.tangent = {static_cast<float>(t.x), static_cast<float>(t.y), static_cast<float>(t.z), 1.0f};
        data.vertices.push_back(v);
        return static_cast<std::uint32_t>(data.vertices.size() - 1);
    }
    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t slot) {
        data.indices.insert(data.indices.end(), {a, b, c});
        data.triangle_slots.push_back(slot);
    }
    /// A quad facing `n` (its winding is made counter-clockwise seen from `n`).
    void quad(math::dvec3 a, math::dvec3 b, math::dvec3 c, math::dvec3 d, math::dvec3 n, std::uint32_t slot) {
        if (math::dot(math::cross(b - a, c - a), n) < 0.0) {
            std::swap(b, d);
        }
        const std::uint32_t i = vertex(a, n);
        const std::uint32_t j = vertex(b, n);
        const std::uint32_t k = vertex(c, n);
        const std::uint32_t l = vertex(d, n);
        triangle(i, j, k, slot);
        triangle(i, k, l, slot);
    }
    /// An axis-aligned box; `inward` faces its inside (a room). `open_face` (0..5: -x +x -y +y -z +z) is left out.
    void box(math::dvec3 lo, math::dvec3 hi, std::uint32_t slot, bool inward = false, int open_face = -1) {
        const double s = inward ? -1.0 : 1.0;
        const auto face = [&](int index, math::dvec3 n, math::dvec3 a, math::dvec3 b, math::dvec3 c, math::dvec3 d) {
            if (index != open_face) {
                quad(a, b, c, d, n * s, slot);
            }
        };
        face(0, {-1, 0, 0}, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z});
        face(1, {1, 0, 0}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z});
        face(2, {0, -1, 0}, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});
        face(3, {0, 1, 0}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z});
        face(4, {0, 0, -1}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, lo.y, lo.z});
        face(5, {0, 0, 1}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
    }
    /// A unit sphere (stacks x slices), its triangles' slots from `band(latitude)`.
    template <class Band>
    void sphere(std::uint32_t stacks, std::uint32_t slices, Band band) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (std::uint32_t i = 0; i <= stacks; ++i) {
            const double theta = pi * static_cast<double>(i) / stacks;  // from +Y
            for (std::uint32_t j = 0; j <= slices; ++j) {
                const double phi = 2.0 * pi * static_cast<double>(j) / slices;
                const math::dvec3 p{std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi)};
                vertex(p, p);
            }
        }
        for (std::uint32_t i = 0; i < stacks; ++i) {
            const std::uint32_t slot = band(pi * 0.5 - pi * (i + 0.5) / stacks);
            for (std::uint32_t j = 0; j < slices; ++j) {
                const std::uint32_t a = base + i * (slices + 1) + j;
                const std::uint32_t b = a + slices + 1;
                triangle(a, b, a + 1, slot);
                triangle(a + 1, b, b + 1, slot);
            }
        }
    }
};

/// A runtime mesh and its materials.
struct Mesh {
    veach::AssetId id;
    std::vector<veach::AssetId> materials;
};

Mesh add_mesh(render::MeshStore& store, Builder&& b, std::vector<veach::AssetId> materials) {
    b.data.materials = materials;
    auto added = store.add(std::move(b.data));
    if (!added) {
        veach::log::error("sp14", added.error());
        return {};
    }
    return {*added, std::move(materials)};
}

veach::AssetId material(render::MeshStore& store, const char* name, math::vec3 color, float roughness,
                        math::vec3 emission = math::vec3(0.0f)) {
    return store.add_material({.name = name,
                               .base_color = math::vec4(color, 1.0f),
                               .metallic = 0.0f,
                               .roughness = roughness,
                               .emissive = emission,
                               .emissive_strength = 1.0f,
                               .double_sided = true});
}

/// Something to draw: a mesh at a position (double, meters, in the scene's frame), rotated and uniformly scaled.
struct Placed {
    const Mesh* mesh = nullptr;
    math::dvec3 position{0.0};
    math::dmat3 rotation{1.0};
    double scale = 1.0;
    bool with_camera = false;  ///< part of the ship: moves with the camera
};

/// The camera-relative world matrix of `p` (the subtraction in double, ADR 0045), or with `float_world` the float32
/// world matrix the engine uses today.
math::mat4 matrix_of(const Placed& p, math::dvec3 camera, bool float_world) {
    math::mat4 m{math::mat3(p.rotation * p.scale)};
    if (float_world) {
        // Today: a float32 Transform in one world frame; the vertex shader then works with large numbers.
        m[3] = math::vec4(math::vec3(p.position), 1.0f);
    } else {
        m[3] = math::vec4(math::vec3(p.position - camera), 1.0f);
    }
    return m;
}

struct Common {
    std::uint32_t width = 960;
    std::uint32_t height = 540;
    float stars = 1.0f;
};

Common common_config() {
    Common c;
    if (const std::optional<std::string> size = veach::env("VEACH_SP14_SIZE")) {
        char* end = nullptr;
        c.width = static_cast<std::uint32_t>(std::strtoul(size->c_str(), &end, 10));
        c.height =
            static_cast<std::uint32_t>(std::strtoul(end != nullptr && *end == 'x' ? end + 1 : "540", nullptr, 10));
    }
    c.stars = static_cast<float>(number("VEACH_SP14_STARS", 1.0));
    return c;
}

/// Rewrites the primary view: the camera at `camera` looking with `rotation`, an infinite reverse-Z projection.
void set_view(render::RenderView& view, math::dvec3 camera, const math::dmat3& rotation, double fov, double near,
              bool float_world) {
    math::mat4 world_from_camera{math::mat3(rotation)};
    if (float_world) {
        world_from_camera[3] = math::vec4(math::vec3(camera), 1.0f);
    }
    view.view = math::inverse(world_from_camera);
    view.projection = math::perspective_reverse_z(static_cast<float>(fov),
                                                  static_cast<float>(view.width) / static_cast<float>(view.height),
                                                  static_cast<float>(near));
    view.view_projection = view.projection * view.view;
    view.inverse_view_projection = math::inverse(view.view_projection);
    view.position = math::vec3(world_from_camera[3]);
    view.near_plane = static_cast<float>(near);
}

void add_draws(render::RenderWorld& out, const std::vector<Placed>& placed, math::dvec3 camera, bool float_world) {
    out.meshes.clear();
    std::uint64_t id = 0x5350313400000000ULL;  // "SP14": not an entity, but stable across frames
    for (const Placed& p : placed) {
        ++id;
        if (p.mesh == nullptr || !p.mesh->id.is_valid()) {
            continue;
        }
        out.meshes.push_back({.mesh = p.mesh->id,
                              .materials = p.mesh->materials,
                              .world = matrix_of(p, camera, float_world),
                              .entity = id});
    }
}

// ---- depth: a cockpit, a gas giant and two moons ----------------------------------------------------------------

struct DepthScene {
    Common common;
    double s = 1.0;
    std::string view = "wide";
    std::optional<double> cap;
    double origin = 0.0;
    bool float_world = false;
    bool check = false;  ///< VEACH_SP14_CHECK=1: count non-finite pixels (the exposure pass, pinned to ev100)
    double move = 0.0;
    double ev100 = 15.0;
    Mesh cockpit, giant, moon, far_moon, shell;
    // Directions from the ship (unit) and distances, radii (m, already scaled).
    math::dvec3 giant_dir, moon_dir, far_dir;
    double giant_distance = 0, giant_radius = 0, moon_distance = 0, moon_radius = 0, far_distance = 0, far_radius = 0;
};

/// `mullion`: where the window's mullion starts (x at 1 m), so it crosses the moon.
Builder cockpit_mesh(double mullion) {
    Builder b;
    // Slots: 0 frame (dark grey), 1 green displays, 2 amber displays, 3 a light grey sensor housing.
    b.box({-0.75, -0.62, -0.95}, {0.75, -0.36, -0.45}, 0);  // the dashboard, its edge 0.45 m away
    b.quad({-0.55, -0.359, -0.85}, {-0.15, -0.359, -0.85}, {-0.15, -0.359, -0.6}, {-0.55, -0.359, -0.6}, {0, 1, 0}, 1);
    b.quad({0.10, -0.359, -0.85}, {0.50, -0.359, -0.85}, {0.50, -0.359, -0.6}, {0.10, -0.359, -0.6}, {0, 1, 0}, 2);
    // The window frame, 1 m out: sill, header, posts and a mullion that crosses the gas giant and the moon.
    b.box({-0.95, -0.38, -1.05}, {0.95, -0.30, -0.98}, 0);
    b.box({-0.95, 0.52, -1.05}, {0.95, 0.60, -0.98}, 0);
    b.box({-0.95, -0.38, -1.05}, {-0.87, 0.60, -0.98}, 0);
    b.box({0.87, -0.38, -1.05}, {0.95, 0.60, -0.98}, 0);
    b.box({mullion, -0.30, -1.02}, {mullion + 0.02, 0.52, -1.0}, 0);
    // Side walls out to 2 m and the nose beyond the window, 1.6 to 2.2 m.
    b.box({-1.25, -0.7, -2.0}, {-1.15, 0.7, 0.3}, 0);
    b.box({1.15, -0.7, -2.0}, {1.25, 0.7, 0.3}, 0);
    b.box({-0.5, -0.75, -2.2}, {0.5, -0.42, -1.6}, 0);
    // A sensor housing 0.1 m from the eye, upper left.
    b.box({-0.075, 0.025, -0.12}, {-0.035, 0.05, -0.10}, 3);
    return b;
}

DepthScene depth_config(render::MeshStore& store) {
    DepthScene d;
    d.common = common_config();
    d.s = number("VEACH_SP14_SCALE", 1.0);
    d.view = veach::env("VEACH_SP14_VIEW").value_or("wide");
    if (veach::env("VEACH_SP14_CAP")) {
        d.cap = number("VEACH_SP14_CAP", 1000.0);
    }
    d.origin = number("VEACH_SP14_ORIGIN", 0.0);
    d.float_world = veach::env_is_one("VEACH_SP14_FLOAT");
    d.check = veach::env_is_one("VEACH_SP14_CHECK");
    d.move = number("VEACH_SP14_MOVE", 0.0);
    d.ev100 = number("VEACH_SP14_EV", 15.0);

    const veach::AssetId hull = material(store, "hull", {0.18f, 0.19f, 0.21f}, 0.55f);
    const veach::AssetId green =
        material(store, "display green", {0.02f, 0.02f, 0.02f}, 0.3f, {300.0f, 2400.0f, 600.0f});
    const veach::AssetId amber =
        material(store, "display amber", {0.02f, 0.02f, 0.02f}, 0.3f, {2400.0f, 1300.0f, 200.0f});
    const veach::AssetId sensor = material(store, "sensor", {0.6f, 0.6f, 0.62f}, 0.4f);

    // A banded gas giant: cream zones and tan, brown and rust belts by latitude.
    std::vector<veach::AssetId> bands;
    for (const math::vec3 c :
         {math::vec3(0.82f, 0.76f, 0.62f), math::vec3(0.66f, 0.52f, 0.38f), math::vec3(0.52f, 0.36f, 0.24f),
          math::vec3(0.74f, 0.62f, 0.48f), math::vec3(0.60f, 0.34f, 0.22f)}) {
        bands.push_back(material(store, "band", c, 0.9f));
    }
    Builder giant;
    giant.sphere(128, 256, [](double latitude) {
        const double x = std::abs(latitude) / deg;
        const int k = static_cast<int>(x / 7.0 + 0.35 * std::sin(latitude * 9.0));
        return static_cast<std::uint32_t>(((k % 5) + 5) % 5);
    });
    d.giant = add_mesh(store, std::move(giant), bands);
    Builder moon;
    moon.sphere(96, 192, [](double) { return 0u; });
    d.moon = add_mesh(store, std::move(moon), {material(store, "moon", {0.13f, 0.13f, 0.13f}, 0.95f)});
    Builder far;
    far.sphere(96, 192, [](double) { return 0u; });
    d.far_moon = add_mesh(store, std::move(far), {material(store, "far moon", {0.30f, 0.27f, 0.24f}, 0.95f)});
    if (d.cap) {
        // The moon's own tessellation, so the two surfaces are parallel: only precision can make them fight.
        Builder shell;
        shell.sphere(96, 192, [](double) { return 0u; });
        d.shell = add_mesh(store, std::move(shell), {material(store, "shell", {0.45f, 0.06f, 0.04f}, 0.95f)});
    }

    // Real distances times s: the gas giant 7e8 m, the moon in front of its lit side at 3.84e8 m, 3.5 deg off its
    // center (inside its 5.7 deg disk); the far moon at 9.5e8 m, centered on the limb so the giant hides half of it.
    d.giant_distance = 7.0e8 * d.s;
    d.giant_radius = 7.0e7 * d.s;
    d.moon_distance = 3.84e8 * d.s;
    d.moon_radius = 1.737e6 * d.s;
    d.far_distance = 9.5e8 * d.s;
    d.far_radius = 2.634e6 * d.s;
    d.giant_dir = forward_of(6.8 * deg, 2.9 * deg);
    d.moon_dir = turned(d.giant_dir, forward_of(30.0 * deg, 8.0 * deg), 3.5 * deg);
    d.far_dir = turned(d.giant_dir, forward_of(14.0 * deg, -4.0 * deg), std::asin(d.giant_radius / d.giant_distance));
    // The mullion ends just left of the moon's center: it hides the moon's left third (its night side).
    d.cockpit = add_mesh(store, cockpit_mesh(d.moon_dir.x / -d.moon_dir.z - 0.0215), {hull, green, amber, sensor});
    return d;
}

void extract_depth(const DepthScene& d, render::RenderWorld& out) {
    if (out.views.empty()) {
        return;
    }
    const double frame = static_cast<double>(out.frame);
    const math::dvec3 start{d.origin, 0.0, 0.0};
    const math::dvec3 camera = start + math::dvec3(d.move * frame, 0.0, 0.0);
    // The bodies stay where they are in the world; the ship (and its cockpit) moves.
    std::vector<Placed> placed;
    if (d.view != "cap") {  // the shell test looks at the moon alone
        placed.push_back({.mesh = &d.cockpit, .position = camera, .with_camera = true});
    }
    placed.push_back({.mesh = &d.giant,
                      .position = start + d.giant_dir * d.giant_distance,
                      .rotation = math::dmat3(math::rotate(math::dmat4(1.0), 0.05, math::dvec3(0.0, 0.0, 1.0))),
                      .scale = d.giant_radius});
    placed.push_back({.mesh = &d.moon, .position = start + d.moon_dir * d.moon_distance, .scale = d.moon_radius});
    placed.push_back({.mesh = &d.far_moon, .position = start + d.far_dir * d.far_distance, .scale = d.far_radius});
    if (d.cap) {
        // A red shell *cap meters above the moon's surface, drawn after it: where the depth test cannot tell them
        // apart, the grey moon shows through.
        placed.push_back(
            {.mesh = &d.shell, .position = start + d.moon_dir * d.moon_distance, .scale = d.moon_radius + *d.cap});
    }
    add_draws(out, placed, camera, d.float_world);

    // The view: the cockpit's forward (-Z), or aimed at a body.
    math::dvec3 aim{0.0, 0.0, -1.0};
    double fov = 70.0 * deg;
    if (d.view == "moon") {
        aim = d.moon_dir;
        fov = 4.0 * deg;
    } else if (d.view == "limb") {
        aim = d.far_dir;
        fov = 2.0 * deg;
    } else if (d.view == "cap") {
        aim = d.moon_dir;
        fov = 0.8 * deg;
    }
    render::RenderView& view = out.views.front();
    set_view(view, camera, rotation_toward(aim), fov, 0.05, d.float_world);
    view.exposure = static_cast<float>(1.0 / (1.2 * std::exp2(d.ev100)));
    if (d.check) {  // the exposure pass's counters (non-finite pixels), at a fixed exposure
        render::AutoExposureSettings fixed;
        fixed.min_ev100 = fixed.max_ev100 = static_cast<float>(d.ev100);
        fixed.report = true;
        view.auto_exposure = fixed;
    }

    // A 1 AU sun from behind the camera and to the right: the bodies mostly lit.
    std::erase_if(out.lights, [](const render::RenderLight& l) { return l.kind == veach::LightKind::directional; });
    render::RenderLight sun{.kind = veach::LightKind::directional,
                            .direction = math::vec3(-math::normalize(math::dvec3(0.6, 0.25, 0.75))),
                            .radiance = math::vec3(static_cast<float>(sun_lux))};
    sun.size = 0.004625f;
    out.lights.push_back(sun);
    out.sky.mode = veach::SkyMode::none;  // deep space
    out.sky.planet.reset();
    if (d.common.stars > 0.0f) {
        out.sky.stars = render::Starfield{.brightness = d.common.stars};
    }
}

// ---- exposure: sunlit ground, a cave, orbit, deep space ---------------------------------------------------------

/// Where the timeline is at one frame.
struct Moment {
    std::string phase;
    math::dvec3 camera{0.0};  ///< m, the ground scene's frame (orbit: straight above it)
    double yaw = 0.0;
    double pitch = 0.0;
    bool orbit = false;
    double sun_visible = 1.0;  ///< the camera's stand-in for shadowing (the interim renderer has no shadows)
    double ambient = 1.0;      ///< the sky's ambient light on surfaces, the same stand-in
    bool headlamp = false;
    bool reset = false;
};

constexpr double ground_sun_elevation = 18.0 * deg;  ///< along +Z, the cave's mouth faces it
constexpr double orbit_sun_elevation = 35.0 * deg;
constexpr double orbit_sun_azimuth = 60.0 * deg;
constexpr double orbit_altitude = 400e3;

/// The cave: a chamber 12 x 10 x 30 m (inside), its mouth at z = 0 facing +Z, the floor on the ground.
constexpr double cave_depth = 20.0;  ///< where the camera stops inside

double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Moment moment_at(std::uint64_t frame) {
    Moment m;
    const auto f = static_cast<double>(frame);
    const auto cave = [&](double z) {
        const double depth = std::max(-z, 0.0);
        m.camera = {0.0, 0.0, z};
        m.sun_visible = 1.0 - smooth(depth / 2.0);
        m.ambient = 0.0003 + 0.9997 * std::exp(-depth / 2.5);
        m.headlamp = true;
    };
    if (frame < 180) {  // A: sunlit ground, the sun 90 deg to the left
        m.phase = "a_ground";
        m.camera = {0.0, 0.0, 20.0};
        m.yaw = 90.0 * deg;
        m.pitch = 3.0 * deg;
        m.reset = frame == 0;
    } else if (frame < 360) {  // B: cut to just inside the cave, walking in
        m.phase = "b_enter";
        cave(-2.0 - (cave_depth - 2.0) * smooth((f - 180.0) / 180.0));
    } else if (frame < 660) {  // C: deep inside, the back wall
        m.phase = "c_cave";
        cave(-cave_depth);
    } else if (frame < 960) {  // D: turn round to the mouth, the sun in it
        m.phase = "d_mouth";
        cave(-cave_depth);
        const double t = smooth((f - 660.0) / 60.0);
        m.yaw = 180.0 * deg * t;
        m.pitch = 9.0 * deg * t;
    } else if (frame < 1260) {  // E: cut to orbit, 400 km up, over the lit limb
        m.phase = "e_orbit";
        m.orbit = true;
        m.yaw = 0.0;
        m.pitch = -12.0 * deg;
    } else if (frame < 1440) {  // F: turn to the sun
        m.phase = "f_sun";
        m.orbit = true;
        const double t = smooth((f - 1260.0) / 60.0);
        m.yaw = orbit_sun_azimuth * t;
        m.pitch = -12.0 * deg + (orbit_sun_elevation + 12.0 * deg) * t;
    } else if (frame < 1950) {  // G: turn away from the sun and the planet: stars only
        m.phase = "g_space";
        m.orbit = true;
        const double t = smooth((f - 1440.0) / 60.0);
        m.yaw = orbit_sun_azimuth + 180.0 * deg * t;
        m.pitch = orbit_sun_elevation + (30.0 * deg - orbit_sun_elevation) * t;
    } else {  // H: cut back to the sunlit ground
        m.phase = "h_ground";
        m.camera = {0.0, 0.0, 20.0};
        m.yaw = 90.0 * deg;
        m.pitch = 3.0 * deg;
    }
    if (m.orbit) {
        m.camera = {0.0, orbit_altitude, 0.0};
        m.sun_visible = 1.0;
        m.ambient = 1.0;
    }
    return m;
}

/// Frames to capture (the frame number Time counts, from 1) and their names.
struct Shot {
    std::uint64_t frame;
    const char* name;
};
constexpr Shot exposure_shots[] = {
    {150, "a_ground"},       {182, "b_cut"},       {240, "b_walk"},      {650, "c_cave"},   {700, "d_turned"},
    {950, "d_mouth"},        {962, "e_cut"},       {1000, "e_orbit_1s"}, {1250, "e_orbit"}, {1430, "f_sun"},
    {1500, "g_turned"},      {1700, "g_space_4s"}, {1940, "g_space"},    {1952, "h_cut"},   {1980, "h_ground_0.5s"},
    {2040, "h_ground_1.5s"}, {2240, "h_ground"},
};

struct ExposureScene {
    Common common;
    double s_p = 1.0;
    vs::AtmosphereRecord record;
    render::AtmosphereParams medium;
    render::AutoExposureSettings settings;
    std::uint64_t from = 0;
    bool hold = false;  ///< stay at frame `from` (timings)
    bool open_cave =
        false;  ///< VEACH_SP14_OPEN_CAVE=1: no sky visibility in the cave (the haze ADR 0082 rule 5 removes)
    std::filesystem::path out;
    Mesh cave;
};

ExposureScene exposure_config(render::MeshStore& store) {
    ExposureScene e;
    e.common = common_config();
    e.s_p = number("VEACH_SP14_SCALE", 1.0);
    e.record = vs::scaled(vs::atmosphere_from(vs::earth_air()), e.s_p, std::sqrt(e.s_p));
    e.medium = sp10_medium_of(e.record);
    e.settings.low_percent = static_cast<float>(number("VEACH_SP14_LOW", e.settings.low_percent));
    e.settings.high_percent = static_cast<float>(number("VEACH_SP14_HIGH", e.settings.high_percent));
    e.settings.clamp_fp16 = !veach::env_is_one("VEACH_SP14_NO_CLAMP");
    e.settings.highlight_max_pull = static_cast<float>(number("VEACH_SP14_PULL", e.settings.highlight_max_pull));
    e.settings.report = true;
    e.from = static_cast<std::uint64_t>(number("VEACH_SP14_FROM", 0.0));
    e.hold = veach::env_is_one("VEACH_SP14_HOLD");
    e.open_cave = veach::env_is_one("VEACH_SP14_OPEN_CAVE");
    e.out = veach::env("VEACH_SP14_OUT").value_or("");

    // The cave: a rock block with a chamber, open at z = 0. Its inside faces inward.
    Builder cave;
    cave.box({-6.0, -eye_height, -30.0}, {6.0, 10.0 - eye_height, 0.0}, 0, true, 5);
    cave.box({-6.5, -eye_height, -30.5}, {6.5, 10.5 - eye_height, 0.0}, 0, false, 5);
    cave.quad({-6.5, -eye_height, 0.0}, {-6.0, -eye_height, 0.0}, {-6.0, 10.5 - eye_height, 0.0},
              {-6.5, 10.5 - eye_height, 0.0}, {0, 0, 1}, 0);
    cave.quad({6.0, -eye_height, 0.0}, {6.5, -eye_height, 0.0}, {6.5, 10.5 - eye_height, 0.0},
              {6.0, 10.5 - eye_height, 0.0}, {0, 0, 1}, 0);
    cave.quad({-6.0, 10.0 - eye_height, 0.0}, {6.0, 10.0 - eye_height, 0.0}, {6.0, 10.5 - eye_height, 0.0},
              {-6.0, 10.5 - eye_height, 0.0}, {0, 0, 1}, 0);
    e.cave = add_mesh(store, std::move(cave), {material(store, "rock", {0.36f, 0.33f, 0.30f}, 0.9f)});
    return e;
}

void extract_exposure(const ExposureScene& e, render::Renderer& renderer, render::RenderWorld& out) {
    if (out.views.empty() || out.frame == 0) {
        return;
    }
    const std::uint64_t frame = e.hold ? e.from : out.frame - 1 + e.from;
    const Moment m = moment_at(frame);
    const math::dvec3 camera = m.camera;
    add_draws(out, {{.mesh = &e.cave}}, camera, false);

    render::RenderView& view = out.views.front();
    set_view(view, camera, rotation_toward(forward_of(m.yaw, m.pitch)), 60.0 * deg, 0.05, false);
    render::AutoExposureSettings settings = e.settings;
    settings.reset = m.reset || (e.from != 0 && out.frame == 1);
    view.auto_exposure = settings;

    // The planet under the cave (+Y up), its center straight below the ground point.
    const double altitude = camera.y + eye_height;
    const math::dvec3 center{0.0, -(e.record.radius + altitude), 0.0};  // camera-relative, m
    const math::dvec3 sun = m.orbit ? forward_of(orbit_sun_azimuth, orbit_sun_elevation)
                                    : math::dvec3(0.0, std::sin(ground_sun_elevation), std::cos(ground_sun_elevation));
    render::PlanetSky p;
    p.medium = e.medium;
    p.center = math::vec3(center * 1e-3);
    p.altitude = static_cast<float>(altitude * 1e-3);
    p.ground_albedo = math::vec3(e.record.ground_albedo);
    p.suns.at(0) = {.direction = math::vec3(sun), .illuminance = math::vec3(static_cast<float>(sun_lux))};
    p.sun_count = 1;
    out.sky.mode = veach::SkyMode::physical;
    out.sky.planet = p;
    // Inside the cave its walls hide the sky (ADR 0082 rule 5): the air and walls within reach of the camera (the
    // chamber, back to the mouth) get no haze and the stand-in's share of the sky's ambient light. The spike knows
    // the chamber; a game measures it (physics::sky_visibility) or marks its interiors.
    if (m.headlamp && !e.open_cave) {
        view.sky_visibility = {.visibility = static_cast<float>(m.ambient),
                               .reach = static_cast<float>(std::max(-camera.z, 0.0) + 2.0)};
    }
    if (e.common.stars > 0.0f) {
        out.sky.stars = render::Starfield{.brightness = e.common.stars};
    }

    std::erase_if(out.lights, [](const render::RenderLight& l) { return l.kind == veach::LightKind::directional; });
    const math::vec3 t = render::atmosphere_transmittance(e.medium, p.altitude, math::vec3(sun));
    render::RenderLight light{.kind = veach::LightKind::directional,
                              .direction = math::vec3(-sun),
                              .radiance = math::vec3(static_cast<float>(sun_lux * m.sun_visible)) * t};
    light.size = 0.004625f;
    out.lights.push_back(light);
    if (m.headlamp) {  // 600 cd, a 20 deg core: a bright caving lamp
        render::RenderLight lamp{.kind = veach::LightKind::spot,
                                 .position = math::vec3(0.0f, -0.1f, 0.0f),
                                 .direction = math::vec3(forward_of(m.yaw, m.pitch - 8.0 * deg)),
                                 .radiance = math::vec3(600.0f, 570.0f, 520.0f)};
        lamp.cos_inner = std::cos(20.0f * static_cast<float>(deg));
        lamp.cos_outer = std::cos(35.0f * static_cast<float>(deg));
        lamp.size = 0.02f;
        out.lights.push_back(lamp);
    }

    if (!e.out.empty()) {
        for (const Shot& shot : exposure_shots) {
            if (shot.frame == frame + 1) {
                std::filesystem::create_directories(e.out);
                renderer.request_screenshot(e.out / (std::string(shot.name) + ".png"));
            }
        }
    }
}

}  // namespace

void register_sp14_spike(veach::World& world) {
    const std::optional<std::string> mode = veach::env("VEACH_SP14");
    if (!mode) {
        return;
    }
    auto& renderer = veach::services(world).get<render::Renderer>();
    render::MeshStore& store = renderer.meshes();
    const Common common = common_config();
    world.entity("sp14_camera").set<veach::Transform>({}).set<veach::Camera>({.near_plane = 0.05f, .ev100 = 15.0f});
    world.system<veach::Window>("Sp14Size")
        .kind<veach::phase::Startup>()
        .each([w = common.width, h = common.height](veach::Window& window) {
            window.width = w;
            window.height = h;
        });
    if (*mode == "depth") {
        auto scene = std::make_shared<DepthScene>(depth_config(store));
        veach::log::info("sp14",
                         "depth at s = {}: gas giant R {:.4g} m at {:.4g} m, moon R {:.4g} m at {:.4g} m, far moon R "
                         "{:.4g} m at {:.4g} m; view {}; camera {} m from the origin, {}",
                         scene->s, scene->giant_radius, scene->giant_distance, scene->moon_radius, scene->moon_distance,
                         scene->far_radius, scene->far_distance, scene->view, scene->origin,
                         scene->float_world ? "float32 world positions" : "camera-relative");
        renderer.add_extractor("sp14_depth",
                               [scene](const veach::World&, render::RenderWorld& out) { extract_depth(*scene, out); });
        return;
    }
    if (*mode == "exposure") {
        auto scene = std::make_shared<ExposureScene>(exposure_config(store));
        renderer.add_extractor("sp14_exposure", [scene, &renderer](const veach::World&, render::RenderWorld& out) {
            extract_exposure(*scene, renderer, out);
        });
        return;
    }
    veach::log::error("sp14", "VEACH_SP14 is '{}': use depth or exposure", *mode);
}

}  // namespace lab
