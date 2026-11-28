// veach: canonical
// Stage `space:body.atmosphere` (atmosphere.hpp). Every draw comes from the stage's hash streams, one per body, so a
// body's air never depends on another body's or on evaluation order (ADR 0044).

#include <veach/space/gen/atmosphere.hpp>

#include <cmath>

namespace space::gen {

namespace {

constexpr double earth_gravity = 9.80665;
constexpr double bar = 101325.0;

void add_gas(AtmosphereComposition& a, std::string_view gas, double fraction) {
    a.gases.at(a.gas_count) = {gas, fraction};
    ++a.gas_count;
}

AtmosphereComposition derive(const veach::procgen::StageContext& ctx, const BodyRecord& b, const BodyPhysics& p) {
    AtmosphereComposition a;
    a.index = b.index;
    const bool solid = b.composition == "space:composition.rocky" || b.composition == "space:composition.icy";
    if (!solid || !p.retains_atmosphere) {
        return a;
    }
    veach::HashStream s = ctx.stream("air", b.index);
    const double g = p.surface_gravity_m_s2;
    const double t_eq = p.equilibrium_temperature_k;
    if (p.potentially_habitable) {
        // Earth-like air: oxygen 17-25 %, a little argon and CO2, nitrogen the rest; 0.7-1.4 bar at Earth's gravity,
        // more on heavier worlds. Greenhouse warming in proportion to the pressure (Earth: 33 K at 1 bar).
        a.kind = atmosphere_temperate;
        const double o2 = s.uniform(0.17, 0.25);
        add_gas(a, gas_n2, 1.0 - o2 - 0.0093 - 0.0004);
        add_gas(a, gas_o2, o2);
        add_gas(a, gas_ar, 0.0093);
        add_gas(a, gas_co2, 0.0004);
        a.surface_pressure_pa = bar * s.uniform(0.7, 1.4) * (g / earth_gravity);
        a.surface_temperature_k = t_eq + 33.0 * a.surface_pressure_pa / bar;
        a.aerosol = aerosol_clear;
        a.aerosol_optical_depth = 0.0053 * s.uniform(0.7, 2.0);  // Hillaire 2020's clear sky: 0.0053
        a.aerosol_scale_height_m = 1200.0;
        a.ozone_column_du = 300.0 * (o2 / 0.2095) * s.uniform(0.8, 1.2);
    } else if (t_eq < 130.0 && p.lightest_retained_molar_mass <= 0.016) {
        // Titan-like: nitrogen with a few percent methane under a photochemical haze.
        a.kind = atmosphere_hazy;
        const double ch4 = s.uniform(0.01, 0.06);
        add_gas(a, gas_n2, 1.0 - ch4 - 0.001);
        add_gas(a, gas_ch4, ch4);
        add_gas(a, gas_h2, 0.001);
        a.surface_pressure_pa = 1.45 * bar * s.uniform(0.5, 2.0) * std::sqrt(g / 1.352);
        a.surface_temperature_k = t_eq + 12.0;
        a.aerosol = aerosol_haze;
        a.aerosol_optical_depth = s.uniform(1.5, 4.0);
        a.aerosol_scale_height_m = 60000.0;
    } else {
        // CO2: thick and hot (Venus) on warm bodies, thin and dusty (Mars) on the rest.
        a.kind = atmosphere_carbon_dioxide;
        add_gas(a, gas_co2, 0.95);
        add_gas(a, gas_n2, 0.03);
        add_gas(a, gas_ar, 0.02);
        if (t_eq > 260.0) {
            a.surface_pressure_pa = 92.0 * bar * s.uniform(0.3, 1.0) * (g / 8.87);
            a.surface_temperature_k = t_eq * s.uniform(2.6, 3.2);
            a.aerosol = aerosol_haze;
            a.aerosol_optical_depth = s.uniform(3.0, 8.0);
            a.aerosol_scale_height_m = 15000.0;
        } else {
            a.surface_pressure_pa = 610.0 * s.uniform(0.5, 3.0) * (g / 3.721);
            a.surface_temperature_k = t_eq + 5.0;
            a.aerosol = aerosol_dust;
            a.aerosol_optical_depth = s.uniform(0.2, 0.8);
            a.aerosol_scale_height_m = 11000.0;
        }
    }
    return a;
}

}  // namespace

AtmosphereSet derive_all_atmospheres(const veach::procgen::StageContext& ctx, const SystemRecord& system,
                                     const std::vector<BodyPhysics>& physics) {
    AtmosphereSet out;
    out.reserve(system.bodies.size());
    for (const BodyRecord& b : system.bodies) {
        out.push_back(derive(ctx, b, physics.at(b.index - 1)));
    }
    return out;
}

std::uint64_t stage_hash(const AtmosphereSet& r) {
    veach::procgen::RecordHasher h(atmosphere_stage_id, atmosphere_stage_version);
    h.add(r.size());
    for (const AtmosphereComposition& a : r) {
        h.add(a.index);
        h.add_id(a.kind);
        h.add(a.gas_count);
        for (std::uint32_t i = 0; i < a.gas_count; ++i) {
            h.add_id(a.gases.at(i).gas);
            h.add_double(a.gases.at(i).fraction);
        }
        h.add_id(a.aerosol);
        for (const double x : {a.surface_pressure_pa, a.surface_temperature_k, a.aerosol_optical_depth,
                               a.aerosol_scale_height_m, a.ozone_column_du}) {
            h.add_double(x);
        }
    }
    return h.value();
}

}  // namespace space::gen
