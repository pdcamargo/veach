#pragma once

// Spike SP-13 (VCH-089), THROWAWAY: a look at the walk/hover/land scene
// (docs/research/spikes/sp-13-walk-hover-land.md). Built into the lab only with the tests (tests/sp13/CMakeLists.txt
// adds this file and VEACH_LAB_SP13). When VEACH_SP13 names a scale, the lab runs the SP-13 Sim with its autopilot to a
// moment, then draws it in the pad's tangent frame (x east, y up, z south) with runtime meshes (ADR 0072): the terrain
// (the colliders' sphere, coarser), the base, the two crates, the two characters as boxes, the craft and a pad marker
// (visual only).
//   VEACH_SP13=s                 body scale (0.01 or 1)
//   VEACH_SP13_AT=hover|landed   the craft hovering at 50 m (default), or landed with its engine off
//   VEACH_SP13=0.01 veach-lab --rhi metal --headless --frames 3 --screenshot out.png

#include <veach/veach.hpp>

namespace lab {

void register_sp13_spike(veach::World& world);

}  // namespace lab
