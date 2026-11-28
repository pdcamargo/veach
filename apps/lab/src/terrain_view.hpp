#pragma once

// The lab's terrain view (docs/specs/voxel-terrain/spec.md voxel.AC10, guide: docs/guides/voxel-terrain.md): a camera
// flies from orbit to 10 m over a planet streamed by engine/voxel, under the travel governor (ADR 0059), as spike
// SP-09 did with its own octree (docs/research/spikes/sp-09-descent.md). Everything terrain is the engine's: a
// procgen field program, a TerrainBody in the planet's reference frame, TerrainModule's streaming, MeshStore upload
// and camera-relative extraction. This module only flies the camera and takes the checkpoints.
//
// The camera carries TerrainDescent (set by scripts/terrain_descent.luau). Every fixed step the camera moves in the
// planet's frame at |v| = max(v_floor, altitude / tau) down to end_altitude, skims there, drops to low_altitude and
// skims again, then the run quits. At each checkpoint (orbit, 100 km, 10 km, 1 km, 100 m, 100 m skim, 10 m) it logs
// the drawn set's weld check (open edges must be 0) and, with VEACH_TERRAIN_OUT=dir, takes a screenshot there.
//   VEACH_TERRAIN_OUT=build/terrain veach-lab --rhi metal --headless --frames 20000 --script
//   scripts/terrain_descent.luau

#include <veach/veach.hpp>

namespace lab {

/// The descent's configuration, on the camera. Scriptable.
struct TerrainDescent {
    double scale = 0.1;           ///< s_p: the planet's radius is 6,371 km x scale
    double start_altitude = 0.0;  ///< meters; 0: max(radius, 200 km)
    double end_altitude = 100.0;
    double low_altitude = 10.0;
    double tau = 5.0;  ///< seconds (ADR 0059)
    double v_floor = 100.0;
    double skim_speed = 100.0;
    double skim_seconds = 10.0;
    double low_seconds = 10.0;
    double lambda = 4.0;
    std::uint32_t budget = 32;  ///< chunk jobs per frame
};

/// What the descent is doing. Scriptable.
struct TerrainDescentState {
    double altitude = 0.0;
    double speed = 0.0;
    std::uint32_t phase = 0;  ///< 0 warm-up, 1 descent, 2 skim, 3 drop, 4 low skim, 5 done
    double time = 0.0;        ///< seconds since the descent started
    std::uint32_t drawn = 0;
    std::uint32_t backlog = 0;
    double lambda = 0.0;  ///< the effective LOD ratio
};

void register_terrain_view(veach::World& world);

}  // namespace lab
