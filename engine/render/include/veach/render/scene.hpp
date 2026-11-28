#pragma once

// Scene components (docs/specs/first-real-scene/spec.md): what gameplay writes
// to put a glTF scene, lights, the sky and photo mode into the world.
// The renderer reads them only during extraction (ADR 0020).

#include <veach/core/ids.hpp>
#include <veach/core/math.hpp>

#include <cstdint>
#include <string>

namespace veach {

/// Spawns the glTF scene at `path` as children of this entity: one per node, with Transform, MeshInstance and
/// lights (ADR 0011). Changing the path respawns them; removing the component despawns them. Scriptable.
struct SceneInstance {
    std::string path;
};

/// A node of an imported scene that has a mesh. Written by SceneInstance; not Scriptable.
struct MeshInstance {
    AssetId scene;
    std::uint32_t node = 0;
};

/// Draws a mesh made at run time (render::MeshStore, docs/specs/runtime-meshes/spec.md) at the entity's
/// FrameTransform, with the materials the mesh was submitted with. The store owns the mesh: removing this component
/// does not free it, and a removed mesh draws nothing. Not Scriptable.
struct RuntimeMeshInstance {
    AssetId mesh;
};

/// Shines down the entity's −Z (ADR 0036). Illuminance in lux. Light sizes (angle, radius) give soft shadows in photo
/// mode; the real-time renderer treats every light as punctual and unshadowed for now (ADR 0040). Scriptable.
struct DirectionalLight {
    math::vec3 color{1.0f};
    float illuminance = 1.0f;
    float angle = 0.0f;  ///< apparent angular diameter, radians (the sun: 0.0093); 0 = a point
};

/// Intensity in candela. Scriptable.
struct PointLight {
    math::vec3 color{1.0f};
    float intensity = 1.0f;
    float radius = 0.0f;  ///< meters: a sphere emitting the same intensity; 0 = a point
};

/// Intensity in candela, cone angles in radians (KHR_lights_punctual falloff). Shines down −Z. Scriptable.
struct SpotLight {
    math::vec3 color{1.0f};
    float intensity = 1.0f;
    float inner_angle = 0.0f;
    float outer_angle = 0.7853982f;
    float radius = 0.0f;  ///< meters, as PointLight's
};

/// The environment (docs/specs/physical-sky/spec.md, ADR 0038). On any one entity; none means the defaults.
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

/// The creator's settings for the space backdrop: the stars, the galactic band and the nebulae beyond every body
/// (docs/specs/space-backdrop/spec.md, ADR 0090). On any one entity; none means the defaults. A module that knows
/// where the camera is in its universe gives the backdrop its content (a star system's, from its address);
/// `seed` makes one here for scenes without such a module. Scriptable.
///
/// Physically a sunlit planet in view makes the stars invisible (they are 13 to 20 stops darker). The backdrop is
/// exposed on its own: `exposure_compensation` stops above its physical brightness in deep space (EV100 -4), and
/// following the camera's exposure only by `exposure_follow`, so it stays visible next to a lit planet. Where the
/// sky itself is bright (a day sky, the limb's glow) it fades back to physical, so day skies stay starless.
struct SpaceBackdrop {
    std::uint32_t seed = 0;              ///< nonzero: a backdrop from this seed (scenes with no module providing one)
    bool visible = true;                 ///< false hides any backdrop
    float exposure_compensation = 4.0f;  ///< stops above physical at EV100 -4
    float exposure_follow = 0.1f;        ///< 0..1: how much a brighter camera exposure dims it (1: physical)
};

/// On a camera entity: photo mode (ADR 0041). The camera's view is path traced by the reference path tracer and
/// accumulates while nothing changes; moving the camera or changing the scene restarts it. Needs hardware ray
/// tracing. Scriptable.
struct PhotoMode {
    std::uint32_t samples_per_frame = 1;
    std::uint32_t max_bounces = 8;
};

}  // namespace veach
