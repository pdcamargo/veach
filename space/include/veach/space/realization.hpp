#pragma once

// veach: canonical
// The realization layer (docs/specs/first-slice/spec.md, ADR 0049, 0088): a universe seed and the creator's settings
// in, a star system to fly in out. It runs the generation stages (space/gen on the engine's stage runtime: G0, S1, B1
// and the atmosphere stage), whose records are physical and never see a scale factor, and only then applies the
// scale:
//
//   s_p  body radii, moon orbits, planetary-band relief (× s_p^relief_exponent)
//   s_o  planet orbits, the star's radius (s_o = s_p by default, never below it: ADR 0068)
//   s_a  atmosphere thickness and heights, coefficients × 1/s_a (s_a = s_p^atmosphere_exponent, 0.5 by default: ADR
//   0081)
//
// Surface gravity stays physical (× the creator's multiplier), so every GM is g R² and every period follows √s. Day
// lengths are physical (× the day multiplier). Canonical: + − × ÷ √ and veach::dmath only, so a realized system is
// the same bits on every platform; the atmosphere's render coefficients are display data (atmosphere.hpp).
//
//   auto system = veach::space::realize({.seed = 42, .system = address, .scale = 0.01});
//   const veach::space::RealizedBody* home = system->home();
//   veach::space::Pose p = system->orbits.evaluate(home->node, tick);

#include <veach/core/error.hpp>
#include <veach/core/math.hpp>
#include <veach/space/atmosphere.hpp>
#include <veach/space/gen/atmosphere.hpp>
#include <veach/space/gen/system.hpp>
#include <veach/space/orbit.hpp>
#include <veach/voxel/density.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veach::space {

/// What a creator sets once, in the universe's genesis (ADR 0055). Defaults: ADR 0066.
struct UniverseConfig {
    std::uint64_t seed = 42;
    std::uint64_t system = 0;          ///< the system's address (space::gen::SystemAddress::value)
    double scale = 0.1;                ///< s_p, in [0.01, 1]
    double orbit_scale = 0.0;          ///< s_o; 0 is s_p. Never below s_p (ADR 0068)
    double atmosphere_exponent = 0.5;  ///< s_a = s_p^atmosphere_exponent (ADR 0081)
    double relief_exponent = 0.5;      ///< planetary-band relief × s_p^relief_exponent (ADR 0049)
    double gravity_multiplier = 1.0;   ///< surface gravity × this
    double day_multiplier = 1.0;       ///< sidereal days × this
    double habitability_bias = 0.5;    ///< G0's creator setting (ADR 0066: habitable planets common)
};

/// The scale factors a config realizes at.
struct Scales {
    double s_p = 1.0;
    double s_o = 1.0;
    double s_a = 1.0;
};
/// Validates the config's scale settings (`space.scale_invalid`): s_p in [0.01, 1], s_o in [s_p, 1], exponents in
/// [0, 1], multipliers positive.
[[nodiscard]] Result<Scales> scale_factors(const UniverseConfig& config);

/// ADR 0049's day-length check: the equator's speed against the surface orbital speed √(gR).
enum class DayCheck : std::uint8_t { ok, warn, refuse };

struct RealizedBody {
    std::uint32_t index = 0;           ///< the body index in its address; 0 is the star
    std::string name;                  ///< "star", or "b" and the index: "b1", "b9"
    std::int32_t node = -1;            ///< its node in StarSystem::orbits
    std::uint32_t parent = 0;          ///< the parent body's index (0: the star; the star's own is 0)
    std::string_view kind;             ///< the star's kind or the body's composition (space::gen ids)
    bool solid = false;                ///< has a surface to stand on (rocky or icy)
    double radius = 0.0;               ///< m, realized
    double surface_gravity = 0.0;      ///< m/s²
    double gm = 0.0;                   ///< g R², m³/s²
    double sphere_of_influence = 0.0;  ///< m, a (m / M)^0.4 with gameplay masses; the star's is infinite
    double relief = 0.0;               ///< m: the planetary band's amplitude, realized
    /// The atmosphere realized at (s_p, s_a); empty for none. Its `top` is from the center.
    std::optional<AtmosphereRecord> atmosphere;
    double atmosphere_top = 0.0;  ///< m above `radius`; 0: no atmosphere
    double equatorial_speed_ratio = 0.0;
    DayCheck day = DayCheck::ok;
    std::uint64_t address = 0;       ///< its body address (space::gen::BodyAddress::value)
    std::uint64_t terrain_seed = 0;  ///< the terrain field's seed: stage_seed(universe, address, "space:body.terrain")
};

/// The generation stages' record hashes. They are the same at every scale (ADR 0049); CI checks it.
struct StageHashes {
    std::uint64_t universe = 0;
    std::uint64_t system = 0;
    std::uint64_t body_physics = 0;
    std::uint64_t atmosphere = 0;
    bool operator==(const StageHashes&) const = default;
};

struct StarSystem {
    UniverseConfig config;
    Scales scale;
    ::space::gen::SystemRecord record;
    std::vector<::space::gen::BodyPhysics> physics;
    ::space::gen::AtmosphereSet atmospheres;
    StageHashes hashes;
    System orbits;                     ///< bodies on rails (ADR 0048): the star at the root, gameplay GMs
    std::vector<RealizedBody> bodies;  ///< the star first, then by body index
    double star_luminosity = 0.0;      ///< W, physical (illuminance uses physical distance, ADR 0049)
    double star_temperature = 0.0;     ///< K

    [[nodiscard]] const RealizedBody* find(std::string_view name) const;
    [[nodiscard]] const RealizedBody* body(std::uint32_t index) const;
    /// The first potentially habitable planet, else the first solid one, else the first planet; null without
    /// planets.
    [[nodiscard]] const RealizedBody* home() const;
    /// The realized geometry (radii, GMs, relief, atmosphere tops, every orbit and spin): it changes with s.
    [[nodiscard]] std::uint64_t realized_hash() const;
    /// The physical distance between two points of the realized system, for illuminance: realized / s_o.
    [[nodiscard]] double physical_distance(double realized_m) const { return realized_m / scale.s_o; }
};

/// Generates the system at `config.system` and realizes it. Fails with space.scale_invalid or space.no_system.
[[nodiscard]] Result<StarSystem> realize(const UniverseConfig& config);

/// Gravity at `r` (body-centered, m) from `body` alone (ADR 0059: the dominant body only), m/s²: inverse-square
/// outside the radius, linear inside.
[[nodiscard]] math::dvec3 gravity(const RealizedBody& body, const math::dvec3& r);

/// The body's terrain density (ADR 0053): a field program (space:terrain.rocky or space:terrain.moon) with the
/// planetary band in planet meters (scaled by s_p through procgen's planet_m_per_m) and the meter band unscaled,
/// on a sphere of the realized radius. Null for bodies without a surface.
[[nodiscard]] Result<std::shared_ptr<const voxel::ProgramDensity>> terrain_density(const StarSystem& system,
                                                                                   const RealizedBody& body);

/// The surface radius along unit direction `n` (bisection on the field, 40 halvings: 1e-8 m at s = 1). Canonical.
[[nodiscard]] double surface_radius(const voxel::ProgramDensity& density, const math::dvec3& n);

}  // namespace veach::space
