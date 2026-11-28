// veach: canonical. A body's terrain field (realization.hpp, terrain_density): procgen field graphs, evaluated by the
// engine's canonical CPU interpreter, on a sphere of the realized radius. The planetary band is in planet meters
// (wavelengths × s_p through planet_m_per_m) with its amplitude already realized (× s_p^relief_exponent); the detail
// band is in meters and never scales (ADR 0049).

#include <veach/space/realization.hpp>

#include <veach/procgen/field_graph.hpp>

#include <format>
#include <string>

namespace veach::space {

namespace {

/// Continents and ridged ranges in planet meters, a 300 m to 2.3 m detail band in meters (SP-09's planet, the lab's
/// terrain view). Material 0 lowland, 1 highland, 2 mountains.
std::string rocky_graph(double relief_m) {
    return std::format(R"({{
      "veach_graph": 1, "id": "space:terrain.rocky", "inputs": {{ "p": "p_planet", "q": "p_local" }},
      "nodes": [
        {{ "id": "continents", "type": "veach:fbm@1", "inputs": {{ "p": "p" }},
          "params": {{ "wavelength": {{ "planet_m": 3000000 }}, "octaves": 6 }} }},
        {{ "id": "mountains", "type": "veach:fbm@1", "inputs": {{ "p": "p" }},
          "params": {{ "wavelength": {{ "planet_m": 600000 }}, "octaves": 5 }} }},
        {{ "id": "ridges", "type": "veach:ridge@1", "inputs": {{ "x": "mountains" }}, "params": {{ "offset": 1.0 }} }},
        {{ "id": "peaks", "type": "veach:remap@1", "inputs": {{ "x": "ridges" }},
          "params": {{ "from": [0.0, 1.0], "to": [0.35, 1.0] }} }},
        {{ "id": "land", "type": "veach:remap@1", "inputs": {{ "x": "continents" }},
          "params": {{ "from": [-1.0, 1.0], "to": [0.0, 0.6] }} }},
        {{ "id": "mask_raw", "type": "veach:remap@1", "inputs": {{ "x": "continents" }},
          "params": {{ "from": [0.05, 0.3], "to": [0.0, 1.0] }} }},
        {{ "id": "mask", "type": "veach:clamp@1", "inputs": {{ "x": "mask_raw" }}, "params": {{ "min": 0.0, "max": 1.0 }} }},
        {{ "id": "height", "type": "veach:blend@1", "inputs": {{ "a": "land", "b": "peaks", "t": "mask" }} }},
        {{ "id": "raise", "type": "veach:remap@1", "inputs": {{ "x": "height" }},
          "params": {{ "from": [0.0, 1.0], "to": [0.0, {0}] }} }},
        {{ "id": "detail", "type": "veach:fbm@1", "inputs": {{ "p": "q" }},
          "params": {{ "wavelength": {{ "m": 300 }}, "octaves": 8, "amplitude": 30 }} }},
        {{ "id": "density", "type": "veach:add@1", "inputs": {{ "a": "raise", "b": "detail" }} }},
        {{ "id": "material", "type": "veach:remap@1", "inputs": {{ "x": "height" }},
          "params": {{ "from": [0.2, 0.8], "to": [0.0, 2.0] }} }}
      ],
      "outputs": {{ "density": "density", "material": "material" }}
    }})",
                       -relief_m);
}

/// A moon: rolling basins in planet meters and a rougher meter band; one material.
std::string moon_graph(double relief_m) {
    return std::format(R"({{
      "veach_graph": 1, "id": "space:terrain.moon", "inputs": {{ "p": "p_planet", "q": "p_local" }},
      "nodes": [
        {{ "id": "basins", "type": "veach:fbm@1", "inputs": {{ "p": "p" }},
          "params": {{ "wavelength": {{ "planet_m": 800000 }}, "octaves": 6 }} }},
        {{ "id": "height", "type": "veach:remap@1", "inputs": {{ "x": "basins" }},
          "params": {{ "from": [-1.0, 1.0], "to": [0.0, {0}] }} }},
        {{ "id": "detail", "type": "veach:fbm@1", "inputs": {{ "p": "q" }},
          "params": {{ "wavelength": {{ "m": 200 }}, "octaves": 6, "amplitude": 12 }} }},
        {{ "id": "density", "type": "veach:add@1", "inputs": {{ "a": "height", "b": "detail" }} }}
      ],
      "outputs": {{ "density": "density" }}
    }})",
                       -relief_m);
}

}  // namespace

Result<std::shared_ptr<const voxel::ProgramDensity>> terrain_density(const StarSystem& system,
                                                                     const RealizedBody& body) {
    if (!body.solid) {
        return std::shared_ptr<const voxel::ProgramDensity>{};
    }
    const bool moon = body.parent != 0;
    const std::string json = moon ? moon_graph(body.relief) : rocky_graph(body.relief);
    auto graph = procgen::parse_field_graph(json, moon ? "space:terrain.moon" : "space:terrain.rocky");
    if (!graph) {
        return std::unexpected(graph.error());
    }
    auto program = procgen::compile(*graph, {.seed = body.terrain_seed, .planet_m_per_m = 1.0 / system.scale.s_p});
    if (!program) {
        return std::unexpected(program.error());
    }
    voxel::ProgramDensitySettings settings{.shape = voxel::BaseShape::sphere,
                                           .radius_m = body.radius,
                                           .density_output = "density",
                                           .material_output = moon ? "" : "material"};
    settings.material = moon ? std::uint8_t{2} : std::uint8_t{0};
    return voxel::ProgramDensity::make(std::move(*program), std::move(settings));
}

}  // namespace veach::space
