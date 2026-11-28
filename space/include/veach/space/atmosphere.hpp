#pragma once

// An atmosphere from its composition (docs/adr/0063-astronomical-rendering.md) and the atmosphere's scale rule of
// docs/adr/0049-scale-model.md (s_a = √s_p). Spike SP-10 (VCH-086), throwaway:
// docs/research/spikes/sp-10-atmosphere.md.
//
// Physical SI units throughout (m, 1/m, Pa, K). The renderer's three channels are the wavelengths of Hillaire 2020
// (680, 550, 440 nm). Rayleigh scattering comes from each gas's refractivity and King factor, absorbers from a
// cross-section and a column, aerosols from a small table (optical depth, single-scattering albedo, phase g).

#include <veach/core/math.hpp>

#include <string_view>
#include <vector>

namespace veach::space {

/// One gas of a mixture, by mole fraction. Names: "N2", "O2", "Ar", "CO2", "CH4", "H2", "He".
struct GasFraction {
    std::string_view gas;
    double fraction = 0.0;
};

/// An aerosol or haze layer: vertical optical depth at 550 nm, an exponential profile, per-channel single-scattering
/// albedo and extinction relative to 550 nm (1: grey, as large dust grains), one Cornette-Shanks g.
struct Aerosol {
    double optical_depth = 0.0;
    double scale_height = 1200.0;          ///< m
    math::dvec3 albedo{0.9};               ///< single-scattering albedo, rgb
    math::dvec3 relative_extinction{1.0};  ///< extinction per channel / extinction at 550 nm
    double g = 0.8;
};

/// An absorbing trace gas (ozone-like) in a tent profile: a column (molecules / m²) and cross-sections (m², rgb).
struct Absorber {
    math::dvec3 cross_section{0.0};
    double column = 0.0;
    double center = 25000.0;      ///< m above the ground
    double half_width = 15000.0;  ///< m
};

/// What a planet's atmosphere is made of. Isothermal: one temperature sets the scale height.
struct Composition {
    std::vector<GasFraction> gases;
    double surface_pressure = 101325.0;  ///< Pa
    double temperature = 288.0;          ///< K
    double gravity = 9.80665;            ///< m/s², at the surface
    double radius = 6.371e6;             ///< m, physical
    Aerosol aerosol;
    Absorber absorber;
    math::dvec3 ground_albedo{0.3};
};

/// The record the renderer consumes (ADR 0063's AtmosphereRecord), physical or scaled.
struct AtmosphereRecord {
    double radius = 0.0;        ///< m, ground radius
    double top = 0.0;           ///< m, from the center
    math::dvec3 rayleigh{0.0};  ///< scattering at the ground, 1/m
    double rayleigh_height = 8000.0;
    math::dvec3 mie_scattering{0.0};
    math::dvec3 mie_absorption{0.0};
    double mie_height = 1200.0;
    double mie_g = 0.8;
    math::dvec3 absorption{0.0};  ///< absorber at the tent's peak, 1/m
    double absorption_center = 25000.0;
    double absorption_half_width = 15000.0;
    math::dvec3 ground_albedo{0.3};
    double number_density = 0.0;       ///< molecules / m³ at the ground
    double mean_molecular_mass = 0.0;  ///< kg
};

/// The render channels' wavelengths, m.
inline constexpr math::dvec3 render_wavelengths{680e-9, 550e-9, 440e-9};

/// The Rayleigh cross-section of one molecule of `gas` at `wavelength` (m²), or 0 for an unknown gas:
/// σ = 24π³ / (λ⁴ N_s²) · ((n_s² − 1) / (n_s² + 2))² · F_K, refractivity n_s − 1 at standard density N_s.
[[nodiscard]] double rayleigh_cross_section(std::string_view gas, double wavelength);

/// The record of a composition, physical.
[[nodiscard]] AtmosphereRecord atmosphere_from(const Composition& composition);

/// ADR 0049: the record realized at body scale s_p, with s_a = √s_p. The radius follows s_p; the atmosphere's
/// thickness and every height follow s_a; coefficients × 1/s_a, so vertical optical depth is unchanged.
[[nodiscard]] AtmosphereRecord scaled(const AtmosphereRecord& physical, double s_p);
/// The same with an explicit s_a (the spike compares s_a = s_p and s_p^0.75 against the rule).
[[nodiscard]] AtmosphereRecord scaled(const AtmosphereRecord& physical, double s_p, double s_a);

/// Vertical optical depth from the ground to the top (analytic: exponential profiles truncated at the top, the
/// absorber's tent), per channel.
[[nodiscard]] math::dvec3 vertical_optical_depth(const AtmosphereRecord& record);

/// An Earth-like N2/O2 atmosphere with Hillaire 2020's clear-sky aerosol and 300 DU of ozone.
[[nodiscard]] Composition earth_air();
/// A Mars-like CO2 atmosphere (610 Pa) with suspended dust (optical depth 0.5).
[[nodiscard]] Composition mars_air();
/// A Titan-like N2/CH4 atmosphere (1.47 bar, 94 K) under a thick orange haze.
[[nodiscard]] Composition titan_air();

/// The visible fraction of a sun's disk (angular radius `sun_radius`) behind an occluding disk (angular radius
/// `occluder_radius`) whose center is `separation` away, all in radians: flat-disk overlap.
[[nodiscard]] double disk_visible_fraction(double sun_radius, double occluder_radius, double separation);

}  // namespace veach::space
