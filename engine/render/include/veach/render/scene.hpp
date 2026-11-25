#pragma once

// Scene components (docs/specs/first-real-scene/spec.md): what gameplay writes
// to put a glTF scene, lights and the reference path tracer into the world.
// The renderer reads them only during extraction (ADR 0016).

#include <veach/core/ids.hpp>
#include <veach/core/math.hpp>

#include <cstdint>
#include <string>

namespace veach {

/// Spawns the glTF scene at `path` as children of this entity: one per node, with Transform, MeshInstance and
/// lights (ADR 0032). Changing the path respawns them; removing the component despawns them. Scriptable.
struct SceneInstance {
    std::string path;
};

/// A node of an imported scene that has a mesh. Written by SceneInstance; not Scriptable.
struct MeshInstance {
    AssetId scene;
    std::uint32_t node = 0;
};

/// Shines down the entity's −Z (ADR 0034). Illuminance in lux. Scriptable.
struct DirectionalLight {
    math::vec3 color{1.0f};
    float illuminance = 1.0f;
};

/// Intensity in candela. Scriptable.
struct PointLight {
    math::vec3 color{1.0f};
    float intensity = 1.0f;
};

/// Intensity in candela, cone angles in radians (KHR_lights_punctual falloff). Shines down −Z. Scriptable.
struct SpotLight {
    math::vec3 color{1.0f};
    float intensity = 1.0f;
    float inner_angle = 0.0f;
    float outer_angle = 0.7853982f;
};

/// On a camera entity: render its view with the reference path tracer (ADR 0034). Scriptable.
/// What the path tracer computes (docs/specs/realtime-renderer-v0/spec.md).
namespace path_tracer_mode {
inline constexpr std::uint32_t full = 0;  ///< all light transport: the reference
inline constexpr std::uint32_t direct =
    1;                                  ///< light reaching the first surface straight from lights; no sky, no bounces
inline constexpr std::uint32_t ao = 2;  ///< debug view: ambient occlusion (1 m) at the first surface, never lighting
}  // namespace path_tracer_mode

/// The real-time renderer's settings, on a camera without a PathTracer (docs/specs/realtime-renderer-v0/spec.md).
/// Without the component, the defaults apply.
struct RealtimeLighting {
    std::uint32_t emissive_samples = 4;  ///< light samples per pixel from emissive triangles
    // STOPGAP until Candela (VCH-017): sky light on diffuse surfaces, darkened by short-range ray-traced AO.
    // Candela replaces it; at most a short-range AO stays. Off for comparisons against direct light.
    bool ambient = true;
    math::vec3 sky{0.0f};       ///< the ambient's sky radiance, nits; also what pixels without geometry show
    std::uint32_t ao_rays = 4;  ///< cosine rays per pixel
    float ao_radius = 1.0f;     ///< meters: occluders farther away do not darken
};

struct PathTracer {
    std::uint32_t samples_per_frame = 1;
    std::uint32_t max_bounces = 8;
    math::vec3 sky{0.0f};    ///< radiance of every ray that escapes, nits
    std::uint32_t mode = 0;  ///< path_tracer_mode: 0 full, 1 direct, 2 ao (debug view)
    bool jitter = true;      ///< false: one ray per pixel center, no anti-aliasing (comparisons with real time)
};

}  // namespace veach
