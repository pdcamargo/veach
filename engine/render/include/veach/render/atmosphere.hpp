#pragma once

// The physical atmosphere on the CPU (ADR 0038, 0063, docs/specs/physical-sky/spec.md,
// docs/specs/planet-atmosphere/spec.md): the sun's position for a time of day, transmittance through the atmosphere
// integrated like the GPU's transmittance LUT, the ray-sphere helpers, and a planet's sky (PlanetSky) for modules
// to fill.

#include <veach/core/math.hpp>
#include <veach/render/scene.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace veach::render {

/// Mirrors AtmosphereParams in shaders/veach/atmosphere.slang. Lengths in kilometers.
struct AtmosphereParams {
    math::vec4 rayleigh{0.0f};        ///< xyz scattering (1/km), w = scale height
    math::vec4 mie{0.0f};             ///< xyz scattering (1/km), w = scale height
    math::vec4 mie_absorption{0.0f};  ///< xyz absorption (1/km), w = phase g
    math::vec4 ozone{0.0f};           ///< xyz absorption (1/km), w = tent center
    math::vec4 planet{0.0f};  ///< x = ground radius, y = top radius, z = ground albedo, w = ozone tent half-width
    bool operator==(const AtmosphereParams&) const = default;
};
static_assert(sizeof(AtmosphereParams) == 80);

/// Hillaire 2020's Earth, scaled by the Sky's density multipliers.
[[nodiscard]] AtmosphereParams atmosphere_params(const Sky& sky);
/// Toward the sun (world, +Y up) for the Sky's time of day, latitude and day of year; north is -Z turned by `north`.
[[nodiscard]] math::vec3 sun_direction(const Sky& sky);
/// Transmittance from `altitude_km` above the ground toward `direction`, to space (0 through the planet): the
/// transmittance LUT's integrator. `direction` is in the local frame, +Y up.
[[nodiscard]] math::vec3 atmosphere_transmittance(const AtmosphereParams& a, float altitude_km, math::vec3 direction);

/// Where origin + t * direction (unit) is inside a sphere about the origin of coordinates, given
/// c = |origin|^2 - radius^2 (at the camera, from its altitude h above radius R: h * (2R + h), which does not cancel).
/// The numerically stable quadratic: both roots keep their precision in kilometer floats at 1e4 km (ADR 0076 decision
/// 6). The twin of sphere_span in shaders/veach/atmosphere.slang.
struct SphereSpan {
    float t0 = 0.0f;  ///< t0 <= t1; either may be negative (behind the origin)
    float t1 = 0.0f;
};
[[nodiscard]] std::optional<SphereSpan> sphere_span(math::vec3 origin, math::vec3 direction, float c);
/// The shadow test: the ray from `p` toward `l` (unit) passes through the sphere of `radius` about the origin of
/// coordinates (it heads inward and passes closer than the radius). A point rounded under the surface does not shadow
/// itself toward a sun above its horizon.
[[nodiscard]] bool sphere_blocks(math::vec3 p, math::vec3 l, float radius);
/// sphere_blocks along a ray: where origin + t * direction (unit) is in the shadow a sphere about the origin of
/// coordinates casts away from `l` (unit), the cylinder of its radius behind its center; c = |origin|^2 - radius^2 as
/// for sphere_span. Empty when the ray never is; an end
/// the ray never reaches is +-1e30. The marches light the part of each step outside it (ADR 0094). The twin of
/// sphere_shadow_span in shaders/veach/atmosphere.slang.
[[nodiscard]] std::optional<SphereSpan> sphere_shadow_span(math::vec3 origin, math::vec3 direction, float c,
                                                           math::vec3 l);

/// The aerial-perspective volume's slice coordinate (0..1) of a fraction x of a ray's span through the air, and its
/// inverse: x = ((1 - cos(pi w)) / 2)^2, slices going as w^4 near the camera and crowding at the far end too. Twins of
/// aerial_w and aerial_x in the shader.
[[nodiscard]] float aerial_slice_w(float x);
[[nodiscard]] float aerial_slice_x(float w);

/// Which media's transmittance and multiple-scattering tables are built (ADR 0076 decision 4): a few slots, each
/// holding one medium's tables, reused least recently used first. A body's tables are rebuilt only when its medium
/// changes or it was evicted, so flying between a planet and its moon and back costs nothing.
class AtmosphereLutCache {
public:
    static constexpr std::size_t slots = 4;
    struct Slot {
        std::size_t index = 0;  ///< which slot's tables to read
        bool build = false;     ///< they hold another medium (or nothing): build them this frame
    };
    /// The slot for `medium` this frame.
    [[nodiscard]] Slot acquire(const AtmosphereParams& medium);
    /// Tables built since the start (a statistic for tests and the log).
    [[nodiscard]] std::uint64_t builds() const { return builds_; }

private:
    struct Entry {
        std::optional<AtmosphereParams> medium;
        std::uint64_t used = 0;
    };
    std::array<Entry, slots> entries_{};
    std::uint64_t clock_ = 0;
    std::uint64_t builds_ = 0;
};

/// One sun of a PlanetSky.
struct PlanetSun {
    math::vec3 direction{0.0f, 1.0f, 0.0f};  ///< toward the sun, world
    math::vec3 illuminance{0.0f};            ///< lux at the top of the atmosphere, per channel
    float angular_radius = 0.004625f;        ///< radians
    bool operator==(const PlanetSun&) const = default;
};

/// A sphere that eclipses the suns (a moon, another planet), evaluated per sample by the sky and the aerial
/// perspective (ADR 0076 decision 2).
struct PlanetOccluder {
    math::vec3 center{0.0f};  ///< relative to the camera, km
    float radius = 0.0f;      ///< km
    math::vec3 albedo{0.12f};
    /// The sky draws it as a sphere lit by the suns, behind the air. Off when the scene draws the body itself.
    bool draw = false;
    bool operator==(const PlanetOccluder&) const = default;
};

/// The sky of a planet around any center, from the ground to orbit (ADR 0063, 0076): a module fills
/// ResolvedSky::planet during extraction; when it is empty the Sky component's path is unchanged. Everything is
/// camera-relative and in kilometers.
struct PlanetSky {
    static constexpr std::size_t max_suns = 4;
    static constexpr std::size_t max_occluders = 4;
    AtmosphereParams medium;  ///< the planet's atmosphere, already at the render scale
    math::vec3 center{0.0f};  ///< the planet's center relative to the camera
    float altitude = 0.0f;    ///< the camera above the ground (not |center| - radius, which cancels)
    math::vec3 ground_albedo{0.3f};
    std::array<PlanetSun, max_suns> suns{};
    std::uint32_t sun_count = 0;
    std::array<PlanetOccluder, max_occluders> occluders{};
    std::uint32_t occluder_count = 0;
    std::uint32_t march =
        0;  ///< 0: the sky-view LUT inside the atmosphere, a per-pixel march outside; 1: always; 2: never
    std::uint32_t march_steps = 16;  ///< ADR 0076 decision 1: 16 looked the same as 32 in every comparison
    bool operator==(const PlanetSky&) const = default;
};

/// A procedural starfield (spike SP-14, VCH-090) drawn as energy-conserving points (ADR 0063), in planet
/// mode (attenuated by the air) or with no sky (deep space). Stands in for the galaxy query's near stars.
struct Starfield {
    float brightness = 1.0f;          ///< 1 is physical (E = 2.5e-6 lux * 10^(-0.4 m)); more is a grading boost
    float limiting_magnitude = 8.0f;  ///< the faintest star drawn
    std::uint32_t cells = 256;        ///< cube-map cells per face side, at most one star each
    bool operator==(const Starfield&) const = default;
};

}  // namespace veach::render
