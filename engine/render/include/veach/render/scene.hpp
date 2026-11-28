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
/// mode. In the real-time renderer the two brightest directional lights cast cascaded shadow maps whose penumbrae
/// follow `angle` (SunShadows); every other light is punctual and unshadowed. Scriptable.
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
/// (docs/specs/space-backdrop/spec.md, ADR 0091). On any one entity; none means the defaults. A module that knows
/// where the camera is in its universe gives the backdrop its content (a star system's, from its address);
/// `seed` makes one here for scenes without such a module. Scriptable.
///
/// Physically a sunlit planet in view makes the stars invisible (they are 13 to 20 stops darker). The backdrop is
/// exposed on its own: `exposure_compensation` stops above its physical brightness in deep space (EV100 -4), and
/// following the camera's exposure only by `exposure_follow`, so it stays visible next to a lit planet. The sky in
/// front hides that boost where it outshines the backdrop both in the world and in the image: whole under a black
/// sky and the faint air around a planet's limb, gone under a day sky (starless), back in twilight with the
/// brightest stars first (docs/specs/space-backdrop/spec.md, "The sky in front hides the boost").
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

/// On a camera entity: how open its surroundings are to the sky (ADR 0082 rule 5, ADR 0091). Air and surfaces nearer
/// than `reach` receive the sky's light times `visibility`, rising to all of it at twice the reach: caves and
/// interiors get no aerial-perspective haze and less of the sky's ambient light, and both return through openings.
/// Gameplay or a module writes it (physics::sky_visibility measures it with ray casts; an interior may set it
/// outright); without it the camera is in the open. Visual only: nothing in the simulation reads it. Scriptable.
struct SkyVisibility {
    float visibility = 1.0f;  ///< 0..1: the fraction of the sky the surroundings leave open
    float reach = 0.0f;       ///< meters from the camera; 0: no enclosure
};

/// On a camera entity: the real-time renderer's screen-space ambient occlusion (ADR 0096), which darkens the sky's
/// ambient light in creases, under overhangs and against walls (never direct light). On by default: a camera without
/// it uses these values. `radius` is meters in the world and never follows scale. Visual only. Scriptable.
struct AmbientOcclusion {
    bool enabled = true;
    float radius = 1.5f;     ///< meters: how far an occluder darkens a surface
    float intensity = 1.0f;  ///< scales the darkening (the power on the visible fraction); 0 turns it off
};

/// On a camera entity: the sun shadows of the real-time renderer (docs/specs/sun-shadows/spec.md, ADR 0095). The
/// brightest directional lights (`suns` of them, at most two) cast cascaded shadow maps from the camera out to
/// `distance` meters, fading out over its last fifth; beyond it, and for every other light, geometry casts no shadow.
/// Distances are meters at every body scale. Without it a camera has these defaults. Photo mode traces its own
/// shadows and ignores it. Scriptable.
struct SunShadows {
    std::uint32_t suns = 2;           ///< 0 turns shadow maps off; at most 2 (ADR 0062)
    float distance = 1000.0f;         ///< meters from the camera where the last cascade ends
    std::uint32_t cascades = 4;       ///< 1 to 4, each four times the reach of the one before
    std::uint32_t resolution = 2048;  ///< texels per side of every cascade, 256 to 4096
};

}  // namespace veach
