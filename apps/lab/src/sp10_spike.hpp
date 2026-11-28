#pragma once

// Spike SP-10 (VCH-086), THROWAWAY: a per-body atmosphere from the ground to orbit
// (docs/research/spikes/sp-10-atmosphere.md). When VEACH_SP10 names a composition (earth, mars, titan; hillaire for
// ADR 0038's Earth; legacy for the same view under the Sky component), the lab puts the camera at the world origin
// above a planet whose center is camera-relative, its up tilted away from world +Y, and hands the renderer a
// render::PlanetSky each frame (an extractor). Configuration, all optional except VEACH_SP10:
//   VEACH_SP10_SCALE=s           s_p (default 1); s_a = sqrt(s_p), or s_p^e with VEACH_SP10_SA_EXPONENT=e
//   VEACH_SP10_ALTITUDE=m        camera altitude in meters; or "orbit" (2 radii up), "top-" / "top+" (just under /
//                                over the atmosphere's top)
//   VEACH_SP10_SUNS=one|system|wide|four   one sun (VEACH_SP10_SUN_ELEVATION, _AZIMUTH in degrees); the two stars of
//                                space::reference_system() at a double sunset; two or four hard-coded suns
//   VEACH_SP10_ECLIPSE=x         a moon over the first sun, its center x * (sum of the two disks' radii) away
//   VEACH_SP10_MARCH=auto|always|never   the per-pixel march outside the atmosphere only, everywhere, or never
//   VEACH_SP10_STEPS=n           steps of the march and of the sky-view LUT (default 32)
//   VEACH_SP10_REBUILD=1         nudge the medium every frame (times the transmittance and multiple-scattering LUTs)
//   VEACH_SP10_EV=ev100  VEACH_SP10_PITCH=deg  VEACH_SP10_YAW=deg  VEACH_SP10_FOV=deg  VEACH_SP10_SIZE=WxH
//   VEACH_SP10_HELMET=1          the Flight Helmet in view (it otherwise stands behind the camera: the renderer
//                                draws nothing without a mesh)
//   VEACH_SP10=mars VEACH_SP10_SCALE=0.01 VEACH_SP10_ALTITUDE=orbit \
//     veach-lab --rhi metal --headless --frames 4 --screenshot out.png

#include <veach/veach.hpp>

namespace lab {

void register_sp10_spike(veach::World& world);

}  // namespace lab
