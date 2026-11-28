// Spike SP-10 (VCH-086), throwaway: an atmosphere from its composition. Not canonical (display only).
#include <veach/space/atmosphere.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace veach::space {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double boltzmann = 1.380649e-23;  ///< J/K
constexpr double loschmidt = 2.6867811e25;  ///< molecules / m³ at 273.15 K and 101325 Pa

struct GasData {
    std::string_view name;
    double refractivity;  ///< n − 1 at 550 nm and the Loschmidt density (dispersion ignored: λ⁻⁴ exactly)
    double king;          ///< depolarization (King) factor
    double molar_mass;    ///< g/mol
};

/// Refractivities: Peck & Khanna 1966 (N2), Zhang et al. 2008 (O2), Peck & Fisher 1964 (Ar), Bideau-Mehu et al.
/// 1973 (CO2), Sneep & Ubachs 2005 (CH4 and King factors), Peck & Hung 1977 (H2), Mansfield & Peck 1969 (He), all at
/// 550 nm, rounded to four digits.
constexpr std::array<GasData, 7> gases{{{"N2", 2.9865e-4, 1.034, 28.0134},
                                        {"O2", 2.7140e-4, 1.096, 31.9988},
                                        {"Ar", 2.8170e-4, 1.000, 39.948},
                                        {"CO2", 4.4900e-4, 1.150, 44.0095},
                                        {"CH4", 4.4100e-4, 1.000, 16.043},
                                        {"H2", 1.3920e-4, 1.020, 2.016},
                                        {"He", 3.4900e-5, 1.000, 4.0026}}};

const GasData* find_gas(std::string_view name) {
    const auto it = std::ranges::find(gases, name, &GasData::name);
    return it == gases.end() ? nullptr : &*it;
}

/// ∫₀^top exp(−h / H) dh.
double exponential_column(double height, double top) {
    return height * (1.0 - std::exp(-top / height));
}

}  // namespace

double rayleigh_cross_section(std::string_view gas, double wavelength) {
    const GasData* g = find_gas(gas);
    if (g == nullptr) {
        return 0.0;
    }
    const double n = 1.0 + g->refractivity;
    const double ratio = (n * n - 1.0) / (n * n + 2.0);
    const double l2 = wavelength * wavelength;
    return 24.0 * pi * pi * pi / (l2 * l2 * loschmidt * loschmidt) * ratio * ratio * g->king;
}

AtmosphereRecord atmosphere_from(const Composition& c) {
    AtmosphereRecord r;
    r.radius = c.radius;
    double total = 0.0;
    double mass = 0.0;
    for (const GasFraction& f : c.gases) {
        if (const GasData* g = find_gas(f.gas)) {
            total += f.fraction;
            mass += f.fraction * g->molar_mass;
        }
    }
    mass = total > 0.0 ? mass / total : 28.97;
    r.mean_molecular_mass = mass * 1e-3 / 6.02214076e23;
    r.number_density = c.surface_pressure / (boltzmann * c.temperature);
    r.rayleigh_height = boltzmann * c.temperature / (r.mean_molecular_mass * c.gravity);
    for (int channel = 0; channel < 3; ++channel) {
        double sigma = 0.0;
        for (const GasFraction& f : c.gases) {
            sigma += f.fraction / std::max(total, 1e-30) * rayleigh_cross_section(f.gas, render_wavelengths[channel]);
        }
        r.rayleigh[channel] = sigma * r.number_density;
    }
    const Aerosol& a = c.aerosol;
    r.mie_height = a.scale_height;
    r.mie_g = a.g;
    const math::dvec3 extinction = a.relative_extinction * (a.optical_depth / a.scale_height);
    r.mie_scattering = extinction * a.albedo;
    r.mie_absorption = extinction * (math::dvec3(1.0) - a.albedo);
    r.absorption_center = c.absorber.center;
    r.absorption_half_width = c.absorber.half_width;
    r.absorption = c.absorber.cross_section * (c.absorber.column / c.absorber.half_width);
    double thickness = std::max(12.0 * r.rayleigh_height, 6.0 * a.scale_height);
    if (c.absorber.column > 0.0) {
        thickness = std::max(thickness, c.absorber.center + c.absorber.half_width);
    }
    r.top = r.radius + thickness;
    r.ground_albedo = c.ground_albedo;
    return r;
}

AtmosphereRecord scaled(const AtmosphereRecord& p, double s_p) {
    return scaled(p, s_p, std::sqrt(s_p));
}

AtmosphereRecord scaled(const AtmosphereRecord& p, double s_p, double s_a) {
    AtmosphereRecord r = p;
    r.radius = p.radius * s_p;
    r.top = r.radius + (p.top - p.radius) * s_a;
    r.rayleigh_height = p.rayleigh_height * s_a;
    r.mie_height = p.mie_height * s_a;
    r.absorption_center = p.absorption_center * s_a;
    r.absorption_half_width = p.absorption_half_width * s_a;
    r.rayleigh = p.rayleigh / s_a;
    r.mie_scattering = p.mie_scattering / s_a;
    r.mie_absorption = p.mie_absorption / s_a;
    r.absorption = p.absorption / s_a;
    return r;
}

math::dvec3 vertical_optical_depth(const AtmosphereRecord& r) {
    const double thickness = r.top - r.radius;
    // The tent clipped to [0, thickness]: integrate its two ramps piecewise.
    const auto tent = [&](double h0, double h1) {
        const double c = r.absorption_center, w = r.absorption_half_width;
        const auto density = [&](double h) {
            return std::max(0.0, 1.0 - std::abs(h - c) / w);
        };
        const int n = 4096;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            sum += density(h0 + (h1 - h0) * (i + 0.5) / n);
        }
        return sum * (h1 - h0) / n;
    };
    return r.rayleigh * exponential_column(r.rayleigh_height, thickness) +
           (r.mie_scattering + r.mie_absorption) * exponential_column(r.mie_height, thickness) +
           r.absorption * tent(0.0, thickness);
}

Composition earth_air() {
    Composition c;
    c.gases = {{"N2", 0.7808}, {"O2", 0.2095}, {"Ar", 0.0093}, {"CO2", 0.0004}};
    // Hillaire 2020's aerosol: scattering 3.996e-6, absorption 4.40e-7 per meter, scale height 1.2 km, g = 0.8.
    c.aerosol = {.optical_depth = 4.436e-6 * 1200.0,
                 .scale_height = 1200.0,
                 .albedo = math::dvec3(3.996 / 4.436),
                 .relative_extinction = math::dvec3(1.0),
                 .g = 0.8};
    // 300 Dobson units (2.687e20 molecules/m² each) in Hillaire's tent. The cross-sections at 680, 550, 440 nm are
    // derived so that column gives Hillaire's peak (0.650, 1.881, 0.085) × 1e-6 per meter; they are within the
    // Chappuis band's published range but not taken from a table.
    c.absorber = {.cross_section = {1.209e-25, 3.500e-25, 1.582e-26},
                  .column = 300.0 * 2.6867e20,
                  .center = 25000.0,
                  .half_width = 15000.0};
    c.ground_albedo = math::dvec3(0.3);
    return c;
}

Composition mars_air() {
    Composition c;
    c.gases = {{"CO2", 0.9532}, {"N2", 0.027}, {"Ar", 0.016}, {"O2", 0.0013}};
    c.surface_pressure = 610.0;
    c.temperature = 210.0;
    c.gravity = 3.721;
    c.radius = 3.3895e6;
    // Dust: optical depth 0.5, grey extinction (micron grains), absorbing in the blue. The albedos were chosen by eye
    // for a butterscotch sky, not taken from a measurement.
    c.aerosol = {.optical_depth = 0.5,
                 .scale_height = 11000.0,
                 .albedo = {0.97, 0.86, 0.62},
                 .relative_extinction = math::dvec3(1.0),
                 .g = 0.65};
    c.absorber = {};
    c.ground_albedo = {0.33, 0.20, 0.12};
    return c;
}

Composition titan_air() {
    Composition c;
    c.gases = {{"N2", 0.984}, {"CH4", 0.0148}, {"H2", 0.0012}};
    c.surface_pressure = 146700.0;
    c.temperature = 94.0;
    c.gravity = 1.352;
    c.radius = 2.5747e6;
    // An orange photochemical haze: small particles (extinction rising toward the blue), absorbing the blue.
    c.aerosol = {.optical_depth = 3.0,
                 .scale_height = 60000.0,
                 .albedo = {0.96, 0.88, 0.62},
                 .relative_extinction = {0.8, 1.0, 1.3},
                 .g = 0.6};
    c.absorber = {};
    c.ground_albedo = {0.20, 0.15, 0.10};
    return c;
}

double disk_visible_fraction(double b, double a, double c) {
    if (c >= a + b) {
        return 1.0;
    }
    if (c <= std::abs(a - b)) {
        return a >= b ? 0.0 : 1.0 - (a * a) / (b * b);
    }
    const double alpha = std::acos(std::clamp((c * c + a * a - b * b) / (2.0 * c * a), -1.0, 1.0));
    const double beta = std::acos(std::clamp((c * c + b * b - a * a) / (2.0 * c * b), -1.0, 1.0));
    const double lens = a * a * (alpha - 0.5 * std::sin(2.0 * alpha)) + b * b * (beta - 0.5 * std::sin(2.0 * beta));
    return std::clamp(1.0 - lens / (pi * b * b), 0.0, 1.0);
}

}  // namespace veach::space
