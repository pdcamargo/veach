#pragma once

// Terrain density sources for the voxel terrain tests and veach-bench-terrain (docs/specs/voxel-terrain/spec.md):
// field graphs compiled by engine/procgen, on a flat world and on spheres.

#include <veach/procgen/field_graph.hpp>
#include <veach/voxel/density.hpp>

#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

namespace veach::test {

inline procgen::FieldProgram compile_graph(const std::string& json, const procgen::BodyConstants& body) {
    auto graph = procgen::parse_field_graph(json, "test graph");
    if (!graph) {
        throw std::runtime_error(graph.error().message);
    }
    auto program = procgen::compile(*graph, body);
    if (!program) {
        throw std::runtime_error(program.error().message);
    }
    return std::move(*program);
}

inline std::shared_ptr<const voxel::ProgramDensity> make_density(procgen::FieldProgram program,
                                                                 const voxel::ProgramDensitySettings& settings) {
    auto d = voxel::ProgramDensity::make(std::move(program), settings);
    if (!d) {
        throw std::runtime_error(d.error().message);
    }
    return *d;
}

/// Rolling hills on a flat world: y = 12 m x fbm(64 m, 4 octaves), materials by height.
inline std::shared_ptr<const voxel::ProgramDensity> flat_hills(std::uint64_t seed = 21) {
    const std::string json = R"({
      "veach_graph": 1, "id": "test:hills", "inputs": { "p": "p_local" },
      "nodes": [
        { "id": "hills", "type": "veach:fbm@1", "inputs": { "p": "p" },
          "params": { "wavelength": { "m": 64 }, "octaves": 4 } },
        { "id": "band", "type": "veach:remap@1", "inputs": { "x": "hills" },
          "params": { "from": [-0.4, 0.4], "to": [0.0, 2.0] } },
        { "id": "material", "type": "veach:clamp@1", "inputs": { "x": "band" }, "params": { "min": 0, "max": 2 } }
      ],
      "outputs": { "height": "hills", "material": "material" }
    })";
    return make_density(compile_graph(json, {.seed = seed}), {.shape = voxel::BaseShape::plane,
                                                              .density_output = "height",
                                                              .density_scale = -12.0,
                                                              .material_output = "material"});
}

/// A lumpy sphere (an asteroid): radius plus 20 m x fbm(80 m, 3 octaves).
inline std::shared_ptr<const voxel::ProgramDensity> lumpy_sphere(double radius_m = 400.0, std::uint64_t seed = 5) {
    const std::string json = R"({
      "veach_graph": 1, "id": "test:lumps", "inputs": { "p": "p_local" },
      "nodes": [
        { "id": "lumps", "type": "veach:fbm@1", "inputs": { "p": "p" },
          "params": { "wavelength": { "m": 80 }, "octaves": 3 } }
      ],
      "outputs": { "lumps": "lumps" }
    })";
    return make_density(
        compile_graph(json, {.seed = seed}),
        {.shape = voxel::BaseShape::sphere, .radius_m = radius_m, .density_output = "lumps", .density_scale = -20.0});
}

/// A planet like SP-09's (docs/research/spikes/sp-09-descent.md): SP-06's continents and ridged mountains in
/// planet meters (6 + 5 octaves, relief_m x s^0.5 at scale s), plus a detail band in meters that never scales
/// (8 octaves from 300 m down to 2.3 m, +-30 m). 19 octaves per sample, against SP-09's 21.
inline std::shared_ptr<const voxel::ProgramDensity> planet(double scale = 1.0, double relief_m = 6000.0,
                                                           std::uint64_t seed = 0x5109) {
    const double relief = relief_m * std::sqrt(scale);
    const std::string json = std::format(R"({{
      "veach_graph": 1, "id": "test:planet", "inputs": {{ "p": "p_planet", "q": "p_local" }},
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
        {{ "id": "material", "type": "veach:remap@1", "inputs": {{ "x": "mask" }},
          "params": {{ "from": [0.0, 1.0], "to": [0.0, 2.0] }} }}
      ],
      "outputs": {{ "density": "density", "material": "material" }}
    }})",
                                         -relief);
    return make_density(compile_graph(json, {.seed = seed, .planet_m_per_m = 1.0 / scale}),
                        {.shape = voxel::BaseShape::sphere,
                         .radius_m = 6'371'000.0 * scale,
                         .density_output = "density",
                         .density_scale = 1.0,
                         .material_output = "material"});
}

}  // namespace veach::test
