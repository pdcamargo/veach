#pragma once

// Spike SP-14 (VCH-090), THROWAWAY: auto-exposure from sunlit ground to a cave to deep space, and camera-relative
// reverse-Z depth from a cockpit to a gas giant (docs/research/spikes/sp-14-exposure-depth.md). When VEACH_SP14 names a
// mode, the lab builds that scene from runtime meshes and draws it from an extractor in camera-relative float32 (the
// subtraction in double, ADR 0045); nothing else in the lab is drawn.
//
//   VEACH_SP14=depth     a cockpit (0.1 to 2 m), a gas giant (R 7e7 m at 7e8 m), a moon in front of it (R 1.737e6 m
//                        at 3.84e8 m) and a second moon behind its limb (R 2.634e6 m at 9.5e8 m), lit by a 1 AU sun,
//                        in deep space with the starfield, at a fixed exposure (VEACH_SP14_EV, default 15)
//     VEACH_SP14_SCALE=s           radii and distances times s (default 1); the cockpit never scales
//     VEACH_SP14_VIEW=wide|moon|limb|cap   the cockpit view (70 deg); the moon over the gas giant (4 deg); the far
//                                  moon at the limb (2 deg); the moon alone (0.8 deg), for the shell test
//     VEACH_SP14_CAP=m             a red shell around the moon, m meters above its surface, with the moon's own
//                                  tessellation (the depth ladder: where they fight, grey shows through)
//     VEACH_SP14_ORIGIN=m          the camera sits m meters from the world origin (default 0)
//     VEACH_SP14_FLOAT=1           positions in float32 world space, as today, instead of camera-relative
//     VEACH_SP14_MOVE=m            the ship moves m meters per frame along +X (S5: 0.01)
//   VEACH_SP14=exposure  a timeline at 60 Hz with auto-exposure (RenderView::auto_exposure), on an Earth-like planet
//                        (spike SP-10's atmosphere): sunlit ground, a cave lit by a headlamp, looking out of the cave
//                        at the sun, a cut to orbit at 400 km over the limb with the starfield, the sun from orbit,
//                        deep space (stars only), then a cut back to the sunlit ground. Each frame's exposure is logged
//                        (category "exposure"); screenshots at the phase marks go to VEACH_SP14_OUT (a directory).
//     VEACH_SP14_SCALE=s           the planet's s_p (default 1; s_a = sqrt(s_p), as SP-10)
//     VEACH_SP14_NO_CLAMP=1        no fp16 clamp of the pre-exposed radiance (shows the sun's overflow)
//     VEACH_SP14_LOW=p VEACH_SP14_HIGH=p   the metered percentile band (default 0.5, 0.9)
//     VEACH_SP14_PULL=stops        how far the highlight percentile may darken the metered exposure (default 4)
//     VEACH_SP14_FROM=f            start the timeline at frame f (with an exposure reset there)
//     VEACH_SP14_HOLD=1            stay at that frame (for timings)
//   Both: VEACH_SP14_SIZE=WxH (default 960x540), VEACH_SP14_STARS=brightness (default 1; 0 = none).
//
//   VEACH_SP14=exposure VEACH_SP14_OUT=out veach-lab --rhi metal --headless --frames 2250

#include <veach/veach.hpp>

namespace lab {

void register_sp14_spike(veach::World& world);

}  // namespace lab
