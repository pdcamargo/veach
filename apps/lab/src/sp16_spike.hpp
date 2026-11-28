#pragma once

// Spike SP-16 (VCH-092), THROWAWAY: the same vegetation field drawn by a GPU-driven visibility buffer (ADR 0060) and
// by GPU-driven forward shading, with and without 4x MSAA, timed per pass on the GPU
// (docs/research/spikes/sp-16-visibility-vs-forward.md). When VEACH_SP16 names a path, the lab builds a 512 m field
// (terrain, about 300,000 instances of grass cards, bushes, trees with masked leaves and rocks: about 7 million
// triangles) in a render feature of its own, culls it on the GPU every frame (frustum, draw distance, last frame's
// HiZ) into one indexed indirect draw per part (VCH-071), and draws it the chosen way; nothing else in the lab draws.
//
//   VEACH_SP16=vis       A: visibility buffer (opaque, then masked), compute material pass
//   VEACH_SP16=forward   B: forward, opaque then masked, the full shader on every masked fragment
//   VEACH_SP16=prepass   B: a depth prepass (alpha-tested), then the full shader once per pixel (depth equal)
//   VEACH_SP16=engine    the engine's own real-time renderer (ADR 0093): models as runtime meshes, instances as
//                        entities, cards in the lab's FieldGrass and FieldLeaves materials; VIEW and SIZE apply
//     VEACH_SP16_MSAA=4            forward and prepass: 4x MSAA in tile memory, alpha to coverage, resolved on tile
//     VEACH_SP16_VIEW=ground|mid|far   eye 1 m above the grass / 18 m above / 90 m above (default ground)
//     VEACH_SP16_SIZE=WxH          default 1920x1080
//     VEACH_SP16_LIGHTS=n          point lights in the shading loop (default 16, at most 64)
//     VEACH_SP16_HIZ=0             no occlusion culling
//     VEACH_SP16_GRASS_RANGE=m     grass draw distance (default 150 m)
//     VEACH_SP16_TIMINGS=n         after VEACH_SP16_WARMUP frames (default 120), log the median GPU ms of each pass
//                                  over n frames (category sp16, a line starting SP16_TIMINGS)
//     VEACH_SP16_STATS=1           log culling counts, masked fragments per pixel and 2x2-quad use, once
//     VEACH_SP16_PAN=deg           turn the camera this much per frame (for shimmer in motion; engine too)
//     VEACH_SP16_STRAFE=m          engine: move the camera this far to the view's right per frame (parallax)
//     VEACH_SP16_TAA=0             engine: the camera without temporal AA (AntiAliasing{temporal = false})
//
// The engine field in motion with and without TAA, against a 16-sample reference (VEACH_SP16_SIZE=7680x4320, box
// filtered): docs/research/spikes/vch-144-taa.md.
//
//   VEACH_SP16=vis VEACH_SP16_VIEW=mid veach-lab --rhi metal --headless --frames 3 --screenshot vis_mid.png

#include <veach/veach.hpp>

namespace lab {

void register_sp16_spike(veach::World& world);

}  // namespace lab
