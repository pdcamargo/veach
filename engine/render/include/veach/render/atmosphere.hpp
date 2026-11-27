#pragma once

// The physical atmosphere on the CPU (ADR 0038, docs/specs/physical-sky/spec.md): the sun's position for a time of
// day, and transmittance through the atmosphere, integrated like the GPU's transmittance LUT.

#include <veach/core/math.hpp>
#include <veach/render/scene.hpp>

namespace veach::render {

/// Mirrors AtmosphereParams in shaders/veach/atmosphere.slang. Lengths in kilometers.
struct AtmosphereParams {
    math::vec4 rayleigh{0.0f};  ///< xyz scattering (1/km), w = scale height
    math::vec4 mie{0.0f};       ///< x scattering, y absorption (1/km), z = scale height, w = phase g
    math::vec4 ozone{0.0f};     ///< xyz absorption (1/km), w = tent center
    math::vec4 planet{0.0f};    ///< x = ground radius, y = top radius, z = ground albedo, w = ozone tent half-width
    bool operator==(const AtmosphereParams&) const = default;
};
static_assert(sizeof(AtmosphereParams) == 64);

/// Hillaire 2020's Earth, scaled by the Sky's density multipliers.
[[nodiscard]] AtmosphereParams atmosphere_params(const Sky& sky);
/// Toward the sun (world, +Y up) for the Sky's time of day, latitude and day of year; north is -Z turned by `north`.
[[nodiscard]] math::vec3 sun_direction(const Sky& sky);
/// Transmittance from `altitude_km` above the ground toward `direction`, to space (0 through the planet): the
/// transmittance LUT's integrator.
[[nodiscard]] math::vec3 atmosphere_transmittance(const AtmosphereParams& a, float altitude_km, math::vec3 direction);

}  // namespace veach::render
