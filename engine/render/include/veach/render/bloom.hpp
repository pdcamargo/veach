#pragma once

// The bloom chain's plan (docs/specs/bloom/spec.md): how many levels the real-time renderer downsamples into and how
// much of the glow each one carries, from the image's size and the camera's Bloom::radius. Level k is the half-size
// image halved k times, so one of its texels covers 2^(k+1) pixels: 2^(k+1) / height of the image's height. The
// weights depend only on that screen-relative size, so a scene glows the same at 1080p and 1440p: a level weighs 1
// from 2 / 1080 of the height (1080p's level 0) up to `radius`, fading to 0 an octave below and above. An image too
// small for the finest of those sizes (under 1080 rows) cannot blur that little; that share of the glow stays in the
// image (`coverage` scales the intensity), so the coarser levels keep the share they have at 1080p.

#include <array>
#include <cstdint>

namespace veach::render {

struct BloomChain {
    static constexpr std::uint32_t max_levels = 12;
    std::uint32_t levels = 0;  ///< 0: no bloom (an image too small)
    /// Each level's share of the glow; they sum to 1.
    std::array<float, max_levels> weights{};
    /// The upsample's blend at each level: its weight over its own and every coarser level's (1 at the last).
    std::array<float, max_levels> blend{};
    /// Each level's texel as a fraction of the image height.
    std::array<float, max_levels> scale{};
    /// The share of the glow these levels carry, of what a chain covering every size would (1 from 1080 rows up):
    /// the display blends in intensity x coverage.
    float coverage = 0.0f;
};

/// The chain for a width x height image and a radius (a fraction of the image height). Levels stop at the first that
/// weighs nothing past the radius, or before one smaller than 2 texels on a side.
[[nodiscard]] BloomChain bloom_chain(std::uint32_t width, std::uint32_t height, float radius);

}  // namespace veach::render
