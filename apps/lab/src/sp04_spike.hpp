#pragma once

// Spike SP-04 (VCH-080), THROWAWAY: the sun from the system lighting the lab's Sky
// (docs/research/spikes/sp-04-orrery-clock.md). When VEACH_SP04_SCALE is set, spawns an Earth-like orrery at that
// scale (veach::space::OrreryModule) paused at a season event, and a SunObserver at 60°N:
//   VEACH_SP04_SCALE=0.01 VEACH_SP04_EVENT=june|september|december VEACH_SP04_TIME=noon|sunset \
//     veach-lab --rhi metal --headless --frames 3 --script scripts/sp04_sky.luau --screenshot out.png
// noon: the observer stands under the sun at the event. sunset: the same observer, the first tick after the event
// where the sun's center reaches the horizon.

#include <veach/veach.hpp>

namespace lab {

void register_sp04_spike(veach::World& world);

}  // namespace lab
