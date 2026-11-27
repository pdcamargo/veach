#pragma once

// Stages S1 (`space:system`) and B1 (`space:body.physics`) of docs/adr/0051-staged-generation-pipeline.md, spike
// SP-05 (VCH-081), throwaway. Records are physical and unscaled (SI units). No stage reads a scale factor: the
// stage context below has no scale field, and realize.hpp is the only code that takes one.
//
// Physics, formulae and sources: docs/research/spikes/sp-05-system-statistics.md and the comments in system.cpp.
// Kinds are stable string ids (ADR 0051: registries by id, never enums).

#include <veach/space/gen/address.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace space::gen {

inline constexpr std::string_view system_stage_id = "space:system";
inline constexpr std::uint32_t system_stage_version = 1;
inline constexpr std::string_view body_physics_stage_id = "space:body.physics";
inline constexpr std::uint32_t body_physics_stage_version = 1;

// Physical constants (CODATA 2018, IAU 2015 nominal values).
namespace si {
inline constexpr double G = 6.67430e-11;              ///< m³ kg⁻¹ s⁻²
inline constexpr double sigma_sb = 5.670374419e-8;    ///< W m⁻² K⁻⁴
inline constexpr double gas_constant = 8.314462618;   ///< J mol⁻¹ K⁻¹
inline constexpr double c = 299792458.0;              ///< m/s
inline constexpr double au = 1.495978707e11;          ///< m
inline constexpr double year = 3.15576e7;             ///< s (Julian)
inline constexpr double gyr = 3.15576e16;             ///< s
inline constexpr double solar_mass = 1.98847e30;      ///< kg
inline constexpr double solar_luminosity = 3.828e26;  ///< W
inline constexpr double solar_radius = 6.957e8;       ///< m
inline constexpr double solar_temperature = 5772.0;   ///< K
inline constexpr double earth_mass = 5.9722e24;       ///< kg
inline constexpr double earth_radius = 6.3710e6;      ///< m (mean)
inline constexpr double jupiter_mass = 1.89813e27;    ///< kg
inline constexpr double pi = 3.14159265358979323846;
}  // namespace si

/// G0 `space:universe`: the part of the universe record these stages read. Creator settings from the genesis are
/// generation inputs; scale factors are not (they live in the realization layer).
struct UniverseRecord {
    std::uint64_t seed = 0;
    /// Probability that a system with a usable habitable zone gets one Earth-sized planet drawn inside it
    /// (research 03 §11.1; ADR 0066: habitable planets common). 0 is physical sampling only.
    double habitability_bias = 0.5;
};

/// What a stage sees: its ancestors and its own address. There is no scale field, on purpose (ADR 0049, 0051).
struct StageContext {
    const UniverseRecord* universe = nullptr;
    SystemAddress address;
};

/// The stage seed of ADR 0050: hash(universe_seed, address_word, hash_id(stage_id), stage_version).
std::uint64_t stage_seed(const StageContext& ctx, std::string_view stage_id, std::uint32_t version);

struct StarRecord {
    std::string_view kind;  ///< "space:star.main_sequence", ".giant", ".white_dwarf", ".neutron_star", ".black_hole"
    std::string_view
        spectral_class;  ///< "O" "B" "A" "F" "G" "K" "M" for (sub)giants and dwarfs; "D" "NS" "BH" for remnants
    double initial_mass_kg = 0;
    double mass_kg = 0;
    double age_s = 0;
    double metallicity_feh = 0;    ///< [Fe/H], dex
    double zams_luminosity_w = 0;  ///< At formation: sets the snow line
    double luminosity_w = 0;       ///< Now: sets the habitable zone
    double radius_m = 0;
    double temperature_k = 0;  ///< Effective
};

struct Orbit {
    double semi_major_axis_m = 0;
    double eccentricity = 0;
    double inclination_rad = 0;
    double ascending_node_rad = 0;
    double argument_of_periapsis_rad = 0;
    double mean_anomaly_at_epoch_rad = 0;
};

/// One body of S1: a planet (parent 0, the primary) or a moon (parent = its planet's body index).
struct BodyRecord {
    std::uint32_t index = 0;       ///< Body index in the address: planets 1..P by orbit, then moons planet by planet
    std::uint32_t parent = 0;      ///< 0 = the primary star
    std::string_view composition;  ///< "space:composition.rocky", ".icy", ".ice_giant", ".gas_giant"
    double mass_kg = 0;
    double radius_m = 0;  ///< S1 needs it for the Roche limit (see the findings: radius moved from B1 to S1)
    double density_kg_m3 = 0;
    Orbit orbit;
    double sidereal_rotation_s = 0;
    double obliquity_rad = 0;
    bool tidally_locked = false;
};

/// S1 `space:system`.
struct SystemRecord {
    SystemAddress address;
    StarRecord star;
    double disk_inner_m = 0;
    double disk_outer_m = 0;
    double snow_line_m = 0;
    double hz_inner_m = 0;             ///< Kopparapu et al. 2014 runaway greenhouse, 1 Earth mass
    double hz_outer_m = 0;             ///< Kopparapu et al. 2014 maximum greenhouse
    bool habitability_anchor = false;  ///< The bias placed a planet in the habitable zone
    std::uint32_t planet_count = 0;
    std::vector<BodyRecord> bodies;  ///< Planets first, in orbit order; then moons
};

/// B1 `space:body.physics` for one body.
struct BodyPhysics {
    std::uint32_t index = 0;
    double bond_albedo = 0;
    double surface_gravity_m_s2 = 0;
    double escape_velocity_m_s = 0;
    double instellation_w_m2 = 0;  ///< Orbit-averaged, at the body (a moon: its planet's orbit)
    double equilibrium_temperature_k = 0;
    double lightest_retained_molar_mass = 0;  ///< kg/mol, Jeans test at the exobase
    bool retains_atmosphere = false;          ///< Cosmic shoreline and Jeans test for CO2
    bool in_habitable_zone = false;
    bool potentially_habitable = false;
    std::string_view temperature_class;  ///< SpaceEngine's: "frigid" "cold" "cool" "temperate" "warm" "hot" "torrid"
};

SystemRecord generate_system(const StageContext& ctx);
BodyPhysics derive_body_physics(const StageContext& ctx, const SystemRecord& system, std::uint32_t body);
std::vector<BodyPhysics> derive_all_body_physics(const StageContext& ctx, const SystemRecord& system);

/// 64-bit hashes over each record's canonical serialization (every field in declaration order, doubles by their
/// bits, every NaN as one value, strings by hash_id). They start from the stage id, version and dmath::version.
std::uint64_t stage_hash(const SystemRecord& r);
std::uint64_t stage_hash(const std::vector<BodyPhysics>& r);

/// Physical-plausibility checks the generator must never fail.
struct Violations {
    std::uint64_t planet_hill = 0;    ///< Adjacent planets closer than 10 mutual Hill radii (periapsis to apoapsis)
    std::uint64_t planet_star = 0;    ///< A planet's periapsis inside the star's fluid Roche limit or 2 stellar radii
    std::uint64_t moon_roche = 0;     ///< A moon's periapsis inside its planet's fluid Roche limit
    std::uint64_t moon_hill = 0;      ///< A moon's apoapsis beyond 0.49 (1 - 1.03 e) Hill radii (Domingos 2006)
    std::uint64_t moon_spacing = 0;   ///< Adjacent moons closer than 10 mutual Hill radii
    std::uint64_t body_overflow = 0;  ///< More than 255 generated bodies
    std::uint64_t non_finite = 0;     ///< NaN or infinite field
    std::uint64_t total() const {
        return planet_hill + planet_star + moon_roche + moon_hill + moon_spacing + body_overflow + non_finite;
    }
    Violations& operator+=(const Violations& o);
};

Violations validate(const SystemRecord& r, const std::vector<BodyPhysics>& physics);

/// Fluid Roche limit, d = 2.44 R_primary (ρ_primary / ρ_satellite)^(1/3).
double roche_limit_m(double primary_radius_m, double primary_density, double satellite_density);
/// Mutual Hill radius ((m1 + m2) / 3M)^(1/3) (a1 + a2) / 2 (Chambers et al. 1996).
double mutual_hill_radius_m(double m1, double m2, double central_mass, double a1, double a2);

}  // namespace space::gen
