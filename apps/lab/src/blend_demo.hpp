#pragma once

// The RHI's blend modes side by side (docs/specs/rhi-textures/spec.md, ADR 0090): one column each of opaque,
// alpha, premultiplied and additive rectangles over colored stripes. The golden image tests/golden/blend_modes.png
// checks every backend draws them alike.

#include <veach/veach.hpp>

namespace lab {

/// Put one on any entity to draw the demo. Scriptable: scripts set how opaque the rectangles are.
struct BlendDemo {
    float opacity = 0.5f;
};

/// Registers BlendDemo, its extractor and the render feature.
void register_blend_demo(veach::World& world);

}  // namespace lab
