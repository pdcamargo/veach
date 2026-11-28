#pragma once

// A body's ground (docs/specs/terrain-materials/spec.md, ADR 0098): which surface layers it shows where, and in which
// colors. The toolbox picks a biome set from the body's composition and climate (temperate, arid, cold, airless),
// places its layers by slope, altitude, depth below the generated surface, temperature and moisture, and paints them
// either with the scans' own colors or with a palette from the planet (its star, its rock), the opt-in procedural
// albedo. The creator chooses with Universe.surface_colors: "" (automatic: scanned colors on habitable planets,
// palettes elsewhere), "scanned" or "palette".
//
// The layers are found by role name in the game's TerrainLayerLibrary (veach/runtime/terrain_surface.hpp); a game
// maps roles to its scans (apps/slice/terrain_layers.json). Without one, surface_layers() gives every role a
// procedural stand-in.
//
// Canonical: the climate fields are a procgen program evaluated on lattice points plus + - * / sqrt, so the vertex
// weights are the same bits on every platform. The palettes are display data.

#include <veach/runtime/terrain_surface.hpp>
#include <veach/space/realization.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace veach::space {

enum class SurfaceColors : std::uint8_t {
    automatic,  ///< scanned colors on potentially habitable planets, palettes on the rest
    scanned,    ///< every layer in its scan's colors
    palette,    ///< every layer in the planet's palette, with the scans' detail
};

/// "" or "auto", "scanned", "palette"; anything else fails with universe.settings_invalid.
[[nodiscard]] Result<SurfaceColors> parse_surface_colors(std::string_view text);

/// The biome sets the toolbox knows.
enum class Biome : std::uint8_t { temperate, arid, cold, airless };
[[nodiscard]] Biome biome_of(const StarSystem& system, const RealizedBody& body);
[[nodiscard]] std::string_view to_string(Biome biome);

/// Every layer role the biomes use, with a procedural fallback each (no scans): the default library's sources and
/// the names a game's library config should give its layers.
[[nodiscard]] std::span<const TerrainLayerSource> surface_layers();

/// The body's surface: its biome's slots found in `library` (a role it lacks takes the nearest one it has, by a
/// fixed preference list, or fails with universe.settings_invalid), its climate fields (field0: temperature, C;
/// field1: moisture, 0 to 1) and its colors.
[[nodiscard]] Result<TerrainSurfaceDesc> surface_desc(const StarSystem& system, const RealizedBody& body,
                                                      const TerrainLayerLibrary& library, SurfaceColors colors);

}  // namespace veach::space
