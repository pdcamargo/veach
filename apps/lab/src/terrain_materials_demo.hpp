#pragma once

// The lab's terrain materials scene (docs/specs/terrain-materials/spec.md): hills and cliffs on a flat voxel world,
// drawn with TerrainMaterial over the procedural layer library, with a dug hole. Set by
// scripts/terrain_materials.luau (the golden) and scripts/terrain_materials_palette.luau.

#include <veach/veach.hpp>

namespace lab {

/// The scene's settings, set once. Scriptable.
struct TerrainMaterialsDemo {
    bool palette = false;  ///< every layer in its palette color (the opt-in procedural albedo)
    bool dig = true;       ///< a hole 5 m across at (dig_x, dig_z)
    double dig_x = 0.0;
    double dig_z = 0.0;
};

void register_terrain_materials_demo(veach::World& world);

}  // namespace lab
