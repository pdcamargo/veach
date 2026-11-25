#pragma once

// First light (docs/specs/first-light/spec.md): the same triangle, drawn either
// by the rasterizer or by a compute shader using ray queries against a TLAS.
// Built only on the public veach:: render API, like any game module would.

#include <veach/veach.hpp>

namespace lab {

enum class TriangleMode : std::uint8_t { raster, raytraced };

/// Put one on any entity to draw the triangle. Scriptable: scripts pick the mode.
struct TriangleDemo {
    TriangleMode mode = TriangleMode::raster;
};

/// Registers TriangleDemo, its extractor and the render feature.
void register_triangle_demo(veach::World& world);

}  // namespace lab
