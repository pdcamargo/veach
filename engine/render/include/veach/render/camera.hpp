#pragma once

// Cameras (docs/specs/render-views/spec.md). A Camera looks down its GlobalTransform's -Z with +Y up,
// through an infinite reverse-Z perspective (ADR 0013). Extraction turns active cameras into
// render::RenderView; gameplay only writes the component.

namespace veach {

struct Camera {
    float fov_y = 1.0471976f;  ///< vertical field of view, radians (60°)
    float near_plane = 0.1f;   ///< meters; the far plane is at infinity
    bool active = true;
    /// Exposure value at ISO 100 (ADR 0037): the image shows radiance x 1 / (1.2 * 2^ev100). 0 derives it from the
    /// physical settings below; their defaults (f/1, 1 s, ISO 100) give 0.
    float ev100 = 0.0f;
    float aperture = 1.0f;  ///< f-number
    float shutter = 1.0f;   ///< seconds
    float iso = 100.0f;
};

/// The camera's EV100: `ev100` if set, else log2(N^2 / t * 100 / S) from aperture, shutter and ISO.
[[nodiscard]] float ev100_of(const Camera& camera);

}  // namespace veach
