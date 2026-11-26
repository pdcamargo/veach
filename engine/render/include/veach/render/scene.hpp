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
inline constexpr std::uint32_t direct_sky = 3;         ///< direct light plus the sky on the first hit's diffuse lobe
inline constexpr std::uint32_t direct_diffuse_gi = 4;  ///< direct light plus diffuse indirect light (Candela's)
}  // namespace path_tracer_mode

/// The real-time renderer's settings, on a camera without a PathTracer (docs/specs/realtime-renderer-v0/spec.md).
/// Without the component, the defaults apply.
/// The environment (docs/specs/physical-sky/spec.md, ADR 0050). On any one entity; none means the defaults.
enum class SkyMode : std::uint8_t {
    physical,  ///< Hillaire's atmosphere: sky light, the sun, aerial perspective
    constant,  ///< `color` in every direction
    none,      ///< black (indoor test scenes)
};

struct Sky {
    SkyMode mode = SkyMode::physical;
    math::vec3 color{0.0f};             ///< constant mode: radiance, nits
    bool drives_sun = true;             ///< physical: the sky's sun replaces the scene's directional lights
    float time_of_day = 12.0f;          ///< hours, local solar time
    float latitude = 45.0f;             ///< degrees
    float day_of_year = 172.0f;         ///< 1..365 (172: the June solstice)
    float north = 0.0f;                 ///< radians: north is -Z turned by this about +Y
    float sun_illuminance = 128000.0f;  ///< lux at the top of the atmosphere
    float ground_albedo = 0.3f;
    float rayleigh = 1.0f;  ///< density multipliers of the Earth defaults
    float mie = 1.0f;
    float ozone = 1.0f;
    float mie_g = 0.8f;
    bool aerial_perspective = true;
};

/// What the real-time renderer shows (docs/specs/temporal-direct-lighting/spec.md).
enum class RealtimeView : std::uint8_t {
    final,        ///< the image
    noisy,        ///< lighting before the denoiser
    motion,       ///< motion vectors: |x|, |y| in red and green (4 render pixels = full)
    history,      ///< green: last frame's surface found; yellow: disoccluded, history borrowed from the same surface
                  ///< next to it; red: none; blue: no geometry
    accumulated,  ///< lighting before the denoiser, averaged over frames while nothing changes (bias checks)
    indirect,     ///< the GI pass's indirect light times the diffuse albedo, exposed and linear, not tonemapped:
                  ///< what Candela (or the stopgap) adds before the denoiser (response measurements)
};

/// The real-time renderer's indirect light (docs/specs/candela/spec.md, ADR 0051).
enum class GlobalIllumination : std::uint8_t {
    off,          ///< GI off: the ambient stopgap, the sky's light darkened by short-range ray-traced AO
    direct_only,  ///< no indirect light at all: direct light only (comparisons with the path tracer's mode 1)
    candela,      ///< Candela (ADR 0051): the path tracer's mode 4 is its reference
};

/// How Candela turns its radiance cache into each pixel's indirect light (ADR 0051).
enum class CandelaGather : std::uint8_t {
    restir,         ///< a ReSTIR GI gather: rays from the visible surface read the cache where they land
    surface_cache,  ///< no rays: the cache's incoming light at the visible surface (a diagnostic, option 3)
};

/// Candela's settings, on the camera; absent: these defaults (docs/specs/candela/spec.md).
struct Candela {
    CandelaGather gather = CandelaGather::restir;
    float ray_rate = 0.25f;             ///< gather rays per render pixel: 0.25, 0.5 or 1
    std::uint32_t spatial_samples = 0;  ///< neighbors' reservoirs reused per pixel, up to 4 (fireflies: VCH-066)
    std::uint32_t cache_bounces = 2;    ///< diffuse surfaces a cache update path visits after the first, up to 3
    bool smooth_cache = true;           ///< cache lookups blend the neighboring cells (no hard steps at cell edges)
};

struct RealtimeLighting {
    std::uint32_t light_candidates = 12;  ///< ReSTIR DI's initial candidates per pixel (ADR 0041, ADR 0046)
    bool temporal_reuse = true;           ///< reuse last frame's reservoirs
    std::uint32_t spatial_samples = 2;    ///< neighbors' reservoirs reused per pixel, up to 8; 0 = none
    bool denoise = true;                  ///< the denoiser (ADR 0042); off for the noisy view and bias checks
    GlobalIllumination gi = GlobalIllumination::off;
    // STOPGAP until Candela (VCH-017), what `gi = off` shows: the sky's light (Sky, ADR 0050) on diffuse surfaces,
    // darkened by short-range ray-traced AO, in its own pass (`gi: ambient`).
    std::uint32_t ao_rays = 4;  ///< cosine rays per pixel
    float ao_radius = 1.0f;     ///< meters: occluders farther away do not darken
    /// Measurement only: parts of shading left out to time what they cost (tools/bench/frame_times.py
    /// --breakdown). 1: no shadow rays (light samples are never shadowed); 2: rays ignore alpha coverage.
    std::uint32_t debug_skip = 0;
    RealtimeView view = RealtimeView::final;
};

/// The quality tier a camera renders at (ADR 0049). A tier sets the render scale, ReSTIR's candidates and neighbors
/// and Candela's ray rate from its budget (engine/render/src/frame_budget.cpp), whatever Upscaling, RealtimeLighting
/// and Candela say; `custom` leaves those to them (benchmarks and comparisons pin their own).
enum class RenderTier : std::uint8_t { performance, quality, custom };

/// On the camera; absent: Performance.
struct RenderQuality {
    RenderTier tier = RenderTier::performance;
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
    /// path_tracer_mode: 0 full, 1 direct, 2 ao (debug view), 3 direct + sky light on the first hit's diffuse lobe
    /// (what the real-time renderer's direct light and ambient stopgap estimate), 4 direct + diffuse indirect light on
    /// it (what direct light and Candela estimate). Escaped rays see the Sky (ADR 0050).
    std::uint32_t mode = 0;
    bool jitter = true;  ///< false: one ray per pixel center, no anti-aliasing (comparisons with real time)
};

}  // namespace veach
