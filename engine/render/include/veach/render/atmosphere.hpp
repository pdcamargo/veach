#pragma once

// The physical atmosphere on the CPU (ADR 0038, docs/specs/physical-sky/spec.md): the sun's position for a time of
// day, and transmittance through the atmosphere, integrated like the GPU's transmittance LUT.

#include <veach/core/math.hpp>
#include <veach/render/scene.hpp>

#include <array>
#include <cstdint>

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

/// Spike SP-10 (VCH-086), throwaway (docs/research/spikes/sp-10-atmosphere.md): one sun of a PlanetSky.
struct PlanetSun {
    math::vec3 direction{0.0f, 1.0f, 0.0f};  ///< toward the sun, world
    math::vec3 illuminance{0.0f};            ///< lux at the top of the atmosphere, per channel
    float angular_radius = 0.004625f;        ///< radians
    bool operator==(const PlanetSun&) const = default;
};

/// Spike SP-10 (VCH-086), throwaway: the sky of a planet around any center, from the ground to orbit, lit by up to
/// four suns, with one spherical occluder (an eclipsing moon). A module fills ResolvedSky::planet during extraction;
/// when it is empty the Sky component's path is unchanged. Everything is camera-relative and in kilometers.
struct PlanetSky {
    AtmosphereParams medium;  ///< the planet's atmosphere, already at the render scale
    math::vec3 center{0.0f};  ///< the planet's center relative to the camera
    float altitude = 0.0f;    ///< the camera above the ground (not |center| - radius, which cancels)
    math::vec3 ground_albedo{0.3f};
    std::array<PlanetSun, 4> suns{};
    std::uint32_t sun_count = 0;
    math::vec4 occluder{0.0f};  ///< xyz = center relative to the camera, w = radius; w = 0: none
    math::vec3 occluder_albedo{0.12f};
    std::uint32_t march =
        0;  ///< 0: the sky-view LUT inside the atmosphere, a per-pixel march outside; 1: always; 2: never
    std::uint32_t march_steps = 32;
    /// Spike SP-14 (VCH-090): the sky's ambient light on surfaces is scaled by this (a cave's stand-in for occlusion).
    float ambient = 1.0f;
    bool operator==(const PlanetSky&) const = default;
};

/// Spike SP-14 (VCH-090), throwaway: a procedural starfield drawn as energy-conserving points (ADR 0063), in planet
/// mode (attenuated by the air) or with no sky (deep space). Stands in for the galaxy query's near stars.
struct Starfield {
    float brightness = 1.0f;          ///< 1 is physical (E = 2.5e-6 lux * 10^(-0.4 m)); more is a grading boost
    float limiting_magnitude = 8.0f;  ///< the faintest star drawn
    std::uint32_t cells = 256;        ///< cube-map cells per face side, at most one star each
    bool operator==(const Starfield&) const = default;
};

}  // namespace veach::render
