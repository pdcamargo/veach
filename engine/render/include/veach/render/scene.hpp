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
    float angle = 0.0f;  ///< apparent angular diameter, radians (the sun: 0.0093); 0 = hard shadows
};

/// Intensity in candela. Scriptable.
struct PointLight {
    math::vec3 color{1.0f};
    float intensity = 1.0f;
    float radius = 0.0f;  ///< meters: a sphere emitting the same intensity; 0 = hard shadows
};

/// Intensity in candela, cone angles in radians (KHR_lights_punctual falloff). Shines down −Z. Scriptable.
struct SpotLight {
    math::vec3 color{1.0f};
    float intensity = 1.0f;
    float inner_angle = 0.0f;
    float outer_angle = 0.7853982f;
    float radius = 0.0f;  ///< meters, as PointLight's
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
/// What the real-time renderer shows (docs/specs/temporal-direct-lighting/spec.md).
enum class RealtimeView : std::uint8_t {
    final,        ///< the image
    noisy,        ///< lighting before the denoiser
    motion,       ///< motion vectors: |x|, |y| in red and green (4 render pixels = full)
    history,      ///< green: last frame's surface found; yellow: disoccluded, history borrowed from the same surface
                  ///< next to it; red: none; blue: no geometry
    accumulated,  ///< lighting before the denoiser, averaged over frames while nothing changes (bias checks)
};

struct RealtimeLighting {
    std::uint32_t light_candidates = 16;  ///< ReSTIR DI's initial candidates per pixel, from every light (ADR 0041)
    bool temporal_reuse = true;           ///< reuse last frame's reservoirs
    std::uint32_t spatial_samples = 4;    ///< neighbors' reservoirs reused per pixel, up to 8; 0 = none
    bool denoise = true;                  ///< the denoiser (ADR 0042); off for the noisy view and bias checks
    // STOPGAP until Candela (VCH-017): sky light on diffuse surfaces, darkened by short-range ray-traced AO.
    // Candela replaces it; at most a short-range AO stays. Off for comparisons against direct light.
    bool ambient = true;
    math::vec3 sky{0.0f};       ///< the ambient's sky radiance, nits; also what pixels without geometry show
    std::uint32_t ao_rays = 4;  ///< cosine rays per pixel
    float ao_radius = 1.0f;     ///< meters: occluders farther away do not darken
    /// Measurement only: parts of shading left out to time what they cost (tools/bench/frame_times.py
    /// --breakdown). 1: no shadow rays (light samples are never shadowed); 2: rays ignore alpha coverage.
    std::uint32_t debug_skip = 0;
    RealtimeView view = RealtimeView::final;
};

/// How the real-time renderer upscales (ADR 0043). `automatic` is MetalFX's temporal scaler where the device
/// supports it, otherwise Veach's own (`temporal`); `none` renders at the output resolution without jitter.
enum class UpscalerMode : std::uint8_t { automatic, metalfx, temporal, none };

/// On the camera: render resolution and upscaler (ADR 0040, ADR 0043). Absent: the defaults.
struct Upscaling {
    UpscalerMode mode = UpscalerMode::automatic;
    float render_scale = 0.75f;  ///< render resolution / output resolution, 0.5..1
};

struct PathTracer {
    std::uint32_t samples_per_frame = 1;
    std::uint32_t max_bounces = 8;
    math::vec3 sky{0.0f};    ///< radiance of every ray that escapes, nits
    std::uint32_t mode = 0;  ///< path_tracer_mode: 0 full, 1 direct, 2 ao (debug view)
    bool jitter = true;      ///< false: one ray per pixel center, no anti-aliasing (comparisons with real time)
};

}  // namespace veach
