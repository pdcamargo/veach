#pragma once

// Cameras (docs/specs/render-views/spec.md). A Camera looks down its GlobalTransform's -Z with +Y up,
// through an infinite reverse-Z perspective (ADR 0010). Extraction turns active cameras into
// render::RenderView; gameplay only writes the component.

namespace veach {

struct Camera {
    float fov_y = 1.0471976f;  ///< vertical field of view, radians (60°)
    float near_plane = 0.1f;   ///< meters; the far plane is at infinity
    bool active = true;
    float ev100 = 0.0f;  ///< exposure (ADR 0034): a pixel shows radiance / (1.2 * 2^ev100)
};

}  // namespace veach
