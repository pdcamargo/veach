#pragma once

// Spike SP-09 (VCH-085), THROWAWAY: a planet's terrain as a body-local chunk octree streamed around a camera that
// descends from orbit under the travel governor (docs/research/spikes/sp-09-descent.md).
//
// The camera entity carries Sp09Descent (the configuration, set by scripts/sp09_descent.luau and overridden from the
// environment, below). Every fixed step the spike moves the camera's double-precision body-local position by
// |v| = max(v_floor, d / tau) (ADR 0059). Every frame it selects a restricted octree of 32^3-cell chunks around it
// (ADR 0053), meshes up to `budget` missing chunks with engine/voxel's marching cubes + Transvoxel on the job system
// (ADR 0073), adds them to render::MeshStore (ADR 0072), and draws the selection relative to the camera (ADR 0045:
// the camera is the render origin, so every chunk's offset is computed in double and only then made float).
//
// Never a hole: the drawn selection switches to the new one only when every chunk of the new one is meshed, so what
// is drawn is always a complete restricted octree whose transition faces match its own neighbors.
//
// Environment (all optional): VEACH_SP09_BODY=planet|asteroid, VEACH_SP09_SCALE, VEACH_SP09_RELIEF (exponent),
// VEACH_SP09_BUDGET (chunks per frame), VEACH_SP09_LAMBDA, VEACH_SP09_SKIM (m/s), VEACH_SP09_COLORS=level,
// VEACH_SP09_ENCODING=linear, VEACH_SP09_OUT (directory for screenshots, frames.csv and summary.json),
// VEACH_SP09_PREFIX (screenshot names), VEACH_SP09_SHOTS=0 (no screenshots), VEACH_SP09_SKY=magenta (hole check),
// VEACH_SP09_BENCH=N (re-mesh the 100 m selection N times and log chunks/s), VEACH_SP09_ADAPTIVE=1 (lower lambda while
// the drawn selection is stale), VEACH_SP09_START (start altitude, meters).

#include <veach/veach.hpp>

namespace lab {

/// The descent's configuration, on the camera entity. Scriptable.
struct Sp09Descent {
    std::uint32_t body = 0;  ///< 0: planet (radius 6,371 km x scale), 1: a 1 km asteroid
    double scale = 0.1;      ///< s_p
    double relief_exponent = 0.5;
    double start_altitude = 0.0;  ///< meters; 0: max(R, 200 km) for the planet, 5 km for the asteroid
    double end_altitude = 100.0;  ///< the descent's end
    double low_altitude = 10.0;   ///< the last skim's altitude
    double tau = 5.0;             ///< seconds (ADR 0059)
    double v_floor = 100.0;       ///< m/s
    double skim_speed = 100.0;    ///< m/s, horizontal, at end_altitude and then at low_altitude
    double skim_seconds = 10.0;
    double low_seconds = 10.0;
    double lambda = 4.0;           ///< LOD ratio (ADR 0053)
    std::uint32_t budget = 32;     ///< chunk jobs per frame
    bool level_colors = false;     ///< color chunks by level instead of by material
    bool linear_encoding = false;  ///< ADR 0053's +-32 m linear samples instead of the companded ones
};

/// What the spike is doing, mirrored for MCP and scripts. Scriptable.
struct Sp09State {
    double x = 0.0;  ///< body-local camera position, meters
    double y = 0.0;
    double z = 0.0;
    double altitude = 0.0;  ///< above the terrain under the camera
    double speed = 0.0;
    std::uint32_t phase = 0;  ///< 0 warm-up, 1 descent, 2 skim, 3 drop, 4 low skim, 5 done
    double time = 0.0;        ///< seconds since the descent started
    std::uint32_t drawn = 0;  ///< chunks drawn
    std::uint32_t backlog = 0;
};

void register_sp09_spike(veach::World& world);

}  // namespace lab
