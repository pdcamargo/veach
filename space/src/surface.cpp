// veach: canonical (the climate fields and the rules; the palettes are display data)
// A body's ground (surface.hpp): biome sets, climate fields, palettes.

#include <veach/space/surface.hpp>

#include <veach/core/hash.hpp>
#include <veach/procgen/field_graph.hpp>
#include <veach/space/gen/atmosphere.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <memory>

namespace veach::space {

namespace {

constexpr float big = std::numeric_limits<float>::max();

TerrainWindow above(float from, float to) {
    return {from, to, big, big};
}
TerrainWindow below(float from, float to) {
    return {-big, -big, from, to};
}
TerrainWindow band(float lo0, float lo1, float hi0, float hi1) {
    return {lo0, lo1, hi0, hi1};
}
/// Untouched ground: not the walls of a dig.
TerrainWindow on_surface() {
    return below(0.12f, 0.35f);
}

/// The climate noise: continental moisture and a temperature wobble, in planet meters (the same pattern at every
/// scale, ADR 0049).
constexpr std::string_view climate_graph = R"({
  "veach_graph": 1, "id": "space:climate", "inputs": { "p": "p_planet" },
  "nodes": [
    { "id": "wet", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "planet_m": 1800000 }, "octaves": 4 } },
    { "id": "wet_fine", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "planet_m": 220000 }, "octaves": 3, "amplitude": 0.35 } },
    { "id": "moisture", "type": "veach:add@1", "inputs": { "a": "wet", "b": "wet_fine" } },
    { "id": "warmth", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "planet_m": 700000 }, "octaves": 3 } }
  ],
  "outputs": { "moisture": "moisture", "warmth": "warmth" }
})";

/// sRGB-ish authoring values to linear.
math::vec3 lin(float r, float g, float b) {
    const auto f = [](float c) {
        return c * c * (0.8f + 0.2f * c);
    };  // close to the sRGB curve, display data
    return {f(r), f(g), f(b)};
}

struct Palette {
    math::vec3 vegetation, rock, sand, soil, snow, ice, salt, lava;
};

/// The planet's palette: vegetation from its star (redder stars, darker and redder pigments) and a pick by the body's
/// address; rock from its address; the rest derived. Display data.
Palette palette_of(const StarSystem& system, const RealizedBody& body) {
    const std::uint64_t h = hash(body.address, hash_id("space:surface.palette"));
    static constexpr std::array<std::array<float, 3>, 6> vegetation{{{0.55f, 0.12f, 0.22f},    // crimson
                                                                     {0.10f, 0.42f, 0.46f},    // teal
                                                                     {0.62f, 0.46f, 0.10f},    // ochre
                                                                     {0.36f, 0.18f, 0.55f},    // violet
                                                                     {0.10f, 0.16f, 0.10f},    // near black
                                                                     {0.70f, 0.30f, 0.12f}}};  // rust orange
    static constexpr std::array<std::array<float, 3>, 5> rock{{{0.50f, 0.27f, 0.18f},          // iron red
                                                               {0.30f, 0.32f, 0.38f},          // slate
                                                               {0.62f, 0.55f, 0.45f},          // pale tan
                                                               {0.22f, 0.20f, 0.20f},          // basalt
                                                               {0.34f, 0.40f, 0.33f}}};        // serpentine
    std::size_t v = h % vegetation.size();
    if (system.star_temperature > 0.0 && system.star_temperature < 4000.0) {
        v = (h & 1u) != 0 ? 0 : 4;  // M dwarfs: crimson or black
    }
    const std::size_t r = (h >> 16) % rock.size();
    Palette p;
    p.vegetation = lin(vegetation.at(v)[0], vegetation.at(v)[1], vegetation.at(v)[2]);
    p.rock = lin(rock.at(r)[0], rock.at(r)[1], rock.at(r)[2]);
    p.sand = lin(std::min(1.0f, rock.at(r)[0] * 1.45f + 0.08f), std::min(1.0f, rock.at(r)[1] * 1.4f + 0.06f),
                 std::min(1.0f, rock.at(r)[2] * 1.3f + 0.04f));
    p.soil = p.rock * 0.45f;
    const bool methane =
        system.atmospheres.size() > body.index && system.atmospheres[body.index].kind == ::space::gen::atmosphere_hazy;
    p.snow = methane ? lin(0.92f, 0.80f, 0.76f) : lin(0.90f, 0.92f, 0.95f);
    p.ice = methane ? lin(0.70f, 0.62f, 0.66f) : lin(0.62f, 0.76f, 0.82f);
    p.salt = lin(0.86f, 0.84f, 0.80f);
    p.lava = p.rock * 0.25f;
    return p;
}

/// One slot: a role, a rule (several rules: the most permissive wins), a palette color.
struct SlotPlan {
    std::string_view role;
    std::vector<TerrainRule> rules;
    math::vec3 palette{0.5f};
    float roughness_scale = 1.0f;
};

/// The roles to try for a role a library lacks, in order.
constexpr std::array<std::array<std::string_view, 4>, 15> role_fallbacks{{
    {"grass", "soil", "rocky_ground", "sand"},
    {"beach_sand", "sand", "red_sand", "gravel"},
    {"cliff", "basalt", "rocky_ground", "sandstone"},
    {"rocky_ground", "gravel", "cliff", "basalt"},
    {"snow", "ice", "salt", "sand"},
    {"soil", "cracked", "sandstone", "gravel"},
    {"red_sand", "sand", "beach_sand", "cracked"},
    {"cracked", "soil", "salt", "sand"},
    {"basalt", "cliff", "rocky_ground", "lava"},
    {"lava", "basalt", "cliff", "rocky_ground"},
    {"salt", "snow", "sand", "cracked"},
    {"sandstone", "cliff", "soil", "rocky_ground"},
    {"ice", "snow", "cliff", "salt"},
    {"gravel", "rocky_ground", "soil", "sand"},
    {"sand", "beach_sand", "red_sand", "gravel"},
}};

std::optional<std::uint32_t> find_role(const TerrainLayerLibrary& library, std::string_view role) {
    if (auto i = library.find(role)) {
        return i;
    }
    for (const auto& chain : role_fallbacks) {
        if (chain[0] != role) {
            continue;
        }
        for (const std::string_view alternative : chain) {
            if (auto i = library.find(alternative)) {
                return i;
            }
        }
    }
    return std::nullopt;
}

std::vector<SlotPlan> plan(Biome biome, double relief, const Palette& p) {
    const auto rel = [&](double fraction) {
        return static_cast<float>(fraction * relief);
    };
    const TerrainRule cliff{.slope = above(0.16f, 0.30f), .depth = on_surface()};
    const TerrainRule deep{.depth = above(1.8f, 3.0f), .strength = 4.0f};
    const TerrainRule subsurface{.depth = band(0.08f, 0.3f, 2.0f, 3.2f), .strength = 4.0f};
    switch (biome) {
        case Biome::temperate:
            return {
                {"grass",
                 {{.slope = below(0.10f, 0.22f),
                   .depth = on_surface(),
                   .field0 = above(-1.0f, 4.0f),
                   .field1 = above(0.20f, 0.30f)}},
                 p.vegetation},
                {"beach_sand",
                 {{.slope = below(0.12f, 0.25f),
                   .depth = on_surface(),
                   .field0 = above(1.0f, 5.0f),
                   .field1 = below(0.20f, 0.30f)},
                  {.altitude = below(rel(0.01), rel(0.035)),
                   .slope = below(0.12f, 0.22f),
                   .depth = on_surface(),
                   .field0 = above(1.0f, 4.0f)}},
                 p.sand},
                {"cliff", {cliff, deep}, p.rock},
                {"rocky_ground",
                 {{.altitude = above(rel(0.42), rel(0.58)),
                   .slope = below(0.22f, 0.32f),
                   .depth = on_surface(),
                   .strength = 1.3f}},
                 p.rock * 1.3f},
                {"snow",
                 {{.slope = below(0.22f, 0.36f),
                   .depth = on_surface(),
                   .field0 = below(-3.0f, 0.0f),
                   .strength = 3.0f}},
                 p.snow},
                {"soil", {subsurface}, p.soil},
            };
        case Biome::arid:
            return {
                {"red_sand",
                 {{.altitude = below(rel(0.45), rel(0.6)),
                   .slope = below(0.14f, 0.24f),
                   .depth = on_surface(),
                   .field1 = below(0.45f, 0.6f)}},
                 p.sand},
                {"cracked",
                 {{.slope = below(0.12f, 0.22f), .depth = on_surface(), .field1 = above(0.45f, 0.6f)}},
                 p.soil * 1.6f},
                {"salt",
                 {{.altitude = below(rel(0.02), rel(0.06)),
                   .slope = below(0.08f, 0.16f),
                   .depth = on_surface(),
                   .strength = 2.5f}},
                 p.salt},
                {"basalt", {cliff, deep}, p.rock},
                {"lava",
                 {{.altitude = above(rel(0.5), rel(0.65)),
                   .slope = below(0.22f, 0.32f),
                   .depth = on_surface(),
                   .strength = 1.5f}},
                 p.lava},
                {"sandstone",
                 {subsurface, {.slope = band(0.10f, 0.16f, 0.22f, 0.30f), .depth = on_surface(), .strength = 0.8f}},
                 p.rock * 1.4f},
            };
        case Biome::cold:
            return {
                {"snow", {{.slope = below(0.18f, 0.30f), .depth = on_surface(), .field1 = above(0.35f, 0.5f)}}, p.snow},
                {"gravel",
                 {{.slope = below(0.18f, 0.30f), .depth = on_surface(), .field1 = below(0.35f, 0.5f)}},
                 p.rock * 1.2f},
                {"rocky_ground",
                 {{.altitude = above(rel(0.45), rel(0.6)),
                   .slope = below(0.24f, 0.32f),
                   .depth = on_surface(),
                   .strength = 1.2f}},
                 p.rock},
                {"ice", {{.slope = band(0.12f, 0.2f, 0.28f, 0.36f), .depth = on_surface()}}, p.ice},
                {"basalt", {cliff, deep}, p.rock * 0.7f},
                {"soil", {subsurface}, p.soil},
            };
        case Biome::airless:
            return {
                {"sand",
                 {{.altitude = below(rel(0.35), rel(0.55)), .slope = below(0.12f, 0.22f), .depth = on_surface()}},
                 p.sand * 0.8f},
                {"gravel",
                 {{.altitude = above(rel(0.3), rel(0.5)), .slope = below(0.2f, 0.3f), .depth = on_surface()}},
                 p.rock * 1.1f},
                {"rocky_ground", {{.slope = band(0.10f, 0.18f, 0.24f, 0.32f), .depth = on_surface()}}, p.rock},
                {"basalt", {cliff, deep, subsurface}, p.rock * 0.6f},
            };
    }
    return {};
}

/// The plans' slots, their roles found in the library.
std::vector<TerrainSlot> slots_of(const std::vector<SlotPlan>& plans, const TerrainLayerLibrary& library, bool palette,
                                  Result<>& status) {
    std::vector<TerrainSlot> out;
    for (const SlotPlan& plan : plans) {
        const auto layer = find_role(library, plan.role);
        if (!layer) {
            status = std::unexpected(Error{ErrorCode::universe_settings_invalid,
                                           std::format("the terrain layer library has no layer for '{}'", plan.role),
                                           "Name the library's layers by the toolbox's roles "
                                           "(veach::space::surface_layers()), or give it a fallback layer."});
            return {};
        }
        out.push_back({.layer = *layer,
                       .rules = plan.rules,
                       .use_palette = palette,
                       .palette = plan.palette,
                       .roughness_scale = plan.roughness_scale});
    }
    return out;
}

}  // namespace

Result<SurfaceColors> parse_surface_colors(std::string_view text) {
    if (text.empty() || text == "auto") {
        return SurfaceColors::automatic;
    }
    if (text == "scanned") {
        return SurfaceColors::scanned;
    }
    if (text == "palette") {
        return SurfaceColors::palette;
    }
    return std::unexpected(Error{ErrorCode::universe_settings_invalid,
                                 std::format("Universe.surface_colors '{}' is not a color mode", text),
                                 "Use \"\" (automatic), \"scanned\" or \"palette\"."});
}

Biome biome_of(const StarSystem& system, const RealizedBody& body) {
    const bool air = system.atmospheres.size() > body.index && system.atmospheres[body.index].present();
    if (!air) {
        return Biome::airless;
    }
    const auto& physics = system.physics;
    const bool habitable = physics.size() > body.index && physics[body.index].potentially_habitable;
    if (habitable || system.atmospheres[body.index].kind == ::space::gen::atmosphere_temperate) {
        return Biome::temperate;
    }
    const double t = system.atmospheres[body.index].surface_temperature_k;
    return t < 250.0 ? Biome::cold : Biome::arid;
}

std::string_view to_string(Biome biome) {
    switch (biome) {
        case Biome::temperate: return "temperate";
        case Biome::arid: return "arid";
        case Biome::cold: return "cold";
        case Biome::airless: return "airless";
    }
    return "?";
}

std::span<const TerrainLayerSource> surface_layers() {
    static const std::vector<TerrainLayerSource> layers = [] {
        const auto layer = [](std::string_view name, math::vec3 color, float roughness, float relief, float grain) {
            TerrainLayerSource s;
            s.name = name;
            s.fallback = {.color = color, .roughness = roughness, .relief = relief, .grain_m = grain};
            return s;
        };
        return std::vector<TerrainLayerSource>{
            layer("grass", {0.30f, 0.40f, 0.16f}, 0.90f, 0.5f, 0.12f),
            layer("beach_sand", {0.72f, 0.64f, 0.48f}, 0.85f, 0.25f, 0.05f),
            layer("cliff", {0.48f, 0.46f, 0.43f}, 0.75f, 1.0f, 0.6f),
            layer("rocky_ground", {0.46f, 0.43f, 0.40f}, 0.80f, 0.8f, 0.25f),
            layer("snow", {0.90f, 0.92f, 0.95f}, 0.55f, 0.25f, 0.4f),
            layer("soil", {0.36f, 0.27f, 0.19f}, 0.92f, 0.5f, 0.15f),
            layer("red_sand", {0.70f, 0.42f, 0.28f}, 0.85f, 0.25f, 0.06f),
            layer("cracked", {0.62f, 0.52f, 0.40f}, 0.90f, 0.6f, 0.3f),
            layer("basalt", {0.25f, 0.24f, 0.24f}, 0.70f, 1.0f, 0.5f),
            layer("lava", {0.18f, 0.16f, 0.16f}, 0.65f, 0.9f, 0.35f),
            layer("salt", {0.86f, 0.84f, 0.80f}, 0.70f, 0.3f, 0.2f),
            layer("sandstone", {0.70f, 0.55f, 0.40f}, 0.80f, 0.8f, 0.4f),
            layer("ice", {0.70f, 0.80f, 0.86f}, 0.35f, 0.6f, 0.5f),
            layer("gravel", {0.50f, 0.48f, 0.45f}, 0.85f, 0.7f, 0.06f),
            layer("sand", {0.76f, 0.66f, 0.48f}, 0.85f, 0.25f, 0.05f),
        };
    }();
    return layers;
}

Result<TerrainSurfaceDesc> surface_desc(const StarSystem& system, const RealizedBody& body,
                                        const TerrainLayerLibrary& library, SurfaceColors colors) {
    const Biome biome = biome_of(system, body);
    const bool palette =
        colors == SurfaceColors::palette || (colors == SurfaceColors::automatic && biome != Biome::temperate);
    const double relief = std::max(body.relief, 1.0);
    Result<> status;
    TerrainSurfaceDesc desc;
    desc.name = std::format("{} {} ({})", body.name, to_string(biome), palette ? "palette" : "scanned");
    desc.slots = slots_of(plan(biome, relief, palette_of(system, body)), library, palette, status);
    if (!status) {
        return std::unexpected(status.error());
    }
    desc.up = TerrainUp::radial;
    desc.datum_m = body.radius;

    // The climate: temperature from the body's mean surface temperature, cooler toward the poles (the spin axis is
    // the body frame's +Y) and with height, plus a wobble; moisture from continental noise, a little drier up high.
    auto graph = procgen::parse_field_graph(climate_graph, "space:climate");
    if (!graph) {
        return std::unexpected(graph.error());
    }
    auto program = procgen::compile(
        *graph, {.seed = hash(body.terrain_seed, hash_id("space:climate")), .planet_m_per_m = 1.0 / system.scale.s_p});
    if (!program) {
        return std::unexpected(program.error());
    }
    double mean_k = system.physics.size() > body.index ? system.physics[body.index].equilibrium_temperature_k : 250.0;
    if (system.atmospheres.size() > body.index && system.atmospheres[body.index].present() &&
        system.atmospheres[body.index].surface_temperature_k > 0.0) {
        mean_k = system.atmospheres[body.index].surface_temperature_k;
    }
    double mean_c = mean_k - 273.15;
    if (biome == Biome::temperate) {
        // The atmosphere stage keeps the equilibrium temperature (no greenhouse warming yet, ADR 0088 rule 2): a
        // temperate world gets Earth's 33 K of it, and stays temperate (grass in the lowlands, snow on peaks and
        // poles).
        mean_c = std::clamp(mean_c + 33.0, 14.0, 24.0);
    }
    desc.name += std::format(", {:.0f} C mean, relief {:.0f} m", mean_c, relief);
    auto climate = std::make_shared<const procgen::FieldProgram>(std::move(*program));
    const double radius = body.radius;
    desc.fields = [climate, mean_c, radius, relief](std::span<const math::dvec3> positions,
                                                    std::span<std::array<float, 2>> out) {
        const procgen::Lattice lattice{.spacing_m = 0.5, .origin_m = {0.0, 0.0, 0.0}};
        std::vector<procgen::LatticePoint> points(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            for (int a = 0; a < 3; ++a) {
                points[i][static_cast<std::size_t>(a)] =
                    static_cast<std::int64_t>(std::floor(positions[i][static_cast<math::length_t>(a)] / 0.5 + 0.5));
            }
        }
        std::vector<float> values(points.size() * 2);
        procgen::evaluate(*climate, lattice, points, values);  // outputs sorted: moisture, warmth
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const math::dvec3& p = positions[i];
            const double r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            const double sin_lat = r > 0.0 ? p.y / r : 0.0;
            const double height = std::clamp((r - radius) / relief, -0.3, 1.5);
            const double moisture = values[i];
            const double warmth = values[positions.size() + i];
            const double t = mean_c + 12.0 - 30.0 * sin_lat * sin_lat - 20.0 * height + 6.0 * warmth;
            out[i] = {static_cast<float>(t),
                      static_cast<float>(std::clamp(0.5 + 0.45 * moisture - 0.15 * height, 0.0, 1.0))};
        }
    };
    return desc;
}

}  // namespace veach::space
