#pragma once

// Spike SP-03 (VCH-079), THROWAWAY: validates ADR 0045's camera-relative extraction without changing engine types.
// See docs/research/spikes/sp-03-precision-frames.md.
//
// An entity with Sp03LargePosition sits at that double-precision offset from the world origin, plus its float32
// GlobalTransform (which stays small: the offset of the entity inside its anchor). A spike extractor, registered
// after the engine's, rewrites the frame's views, draws and lights relative to the primary camera, subtracting the
// large offsets in double and casting the result to float32. The GPU never sees an absolute position.
//
// The lab script scripts/sp03_ladder.luau reads its case from Sp03Ladder, which this module spawns only when
// VEACH_SP03_DISTANCE is set (meters; VEACH_SP03_MODE = float | double, VEACH_SP03_PHOTO = 1 for photo mode).

#include <veach/veach.hpp>

namespace lab {

/// A double-precision offset from the world origin, added to the entity's (and its subtree's) GlobalTransform.
struct Sp03LargePosition {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/// The ladder case a run renders.
struct Sp03Ladder {
    double distance = 0.0;  ///< meters from the world origin
    bool large = false;     ///< true: Sp03LargePosition + camera-relative extraction; false: float32 Transform
    bool photo = false;     ///< photo mode (the path tracer: TLAS instances from MeshDraw world matrices)
};

void register_sp03_spike(veach::World& world);

}  // namespace lab
