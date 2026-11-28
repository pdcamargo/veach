// veach: canonical
//
// S1 `space:system` and B1 `space:body.physics`, spike SP-05 (VCH-081), throwaway. Canonical tier (ADR 0043): only
// + - * / sqrt, comparisons, and veach::dmath for transcendentals; randomness only from veach::HashStream (ADR 0044).
//
// The model is direct sampling of calibrated distributions with physical constraints enforced by construction
// (research 03 §7.3 and "what would change my mind"), not a disc simulation. Sources are cited where each formula is
// used; "(unverified)" marks values taken from standard references without fetching them, as in research 03.

#include <veach/space/gen/system.hpp>

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/procgen/stage.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace space::gen {

namespace dm = veach::dmath;
using veach::HashStream;

namespace {

constexpr std::string_view star_main_sequence = "space:star.main_sequence";
constexpr std::string_view star_giant = "space:star.giant";
constexpr std::string_view star_white_dwarf = "space:star.white_dwarf";
constexpr std::string_view star_neutron = "space:star.neutron_star";
constexpr std::string_view star_black_hole = "space:star.black_hole";

constexpr std::string_view comp_rocky = "space:composition.rocky";
constexpr std::string_view comp_icy = "space:composition.icy";
constexpr std::string_view comp_ice_giant = "space:composition.ice_giant";
constexpr std::string_view comp_gas_giant = "space:composition.gas_giant";

constexpr double solar_constant = 1361.0;  ///< W/m², Earth's instellation

/// Generation targets K ≥ 11 mutual Hill radii and validation requires ≥ 10 (Chambers et al. 1996: ≳ 10 for Gyr
/// stability of multi-planet systems, unverified), so rounding can never produce a violation.
constexpr double hill_spacing_generate = 11.0;
constexpr double hill_spacing_required = 10.0;

// ---- draws (all from HashStreams) ----

double log_uniform(HashStream& s, double lo, double hi) {
    return lo * dm::exp(s.next_double() * dm::log(hi / lo));
}

double gaussian(HashStream& s) {              // Box-Muller, one value
    const double u1 = 1.0 - s.next_double();  // (0, 1]
    const double u2 = s.next_double();
    return std::sqrt(-2.0 * dm::log(u1)) * dm::cos(2.0 * si::pi * u2);
}

double rayleigh(HashStream& s, double sigma) {
    return sigma * std::sqrt(-2.0 * dm::log(1.0 - s.next_double()));
}

double exponential(HashStream& s, double mean) {
    return -mean * dm::log(1.0 - s.next_double());
}

/// Sample dN/dm ∝ m^-alpha on [lo, hi] by inverting the CDF.
double power_law(double u, double lo, double hi, double alpha) {
    const double e = 1.0 - alpha;
    const double a = dm::pow(lo, e), b = dm::pow(hi, e);
    return dm::pow(a + u * (b - a), 1.0 / e);
}

/// Kroupa 2001 IMF (unverified): dN/dm ∝ m^-1.3 on [0.08, 0.5) M☉ and m^-2.3 on [0.5, 100] M☉, continuous at 0.5.
/// Brown dwarfs (< 0.08 M☉, slope 0.3) are not systems with a star here.
double sample_imf_solar(HashStream& s) {
    static const double w_low = (dm::pow(0.08, -0.3) - dm::pow(0.5, -0.3)) / 0.3;
    static const double w_high = 0.5 * (dm::pow(0.5, -1.3) - dm::pow(100.0, -1.3)) / 1.3;
    const double pick = s.next_double() * (w_low + w_high);
    const double u = s.next_double();
    return pick < w_low ? power_law(u, 0.08, 0.5, 1.3) : power_law(u, 0.5, 100.0, 2.3);
}

// ---- stars ----

/// Main-sequence mass-luminosity relation (Duric 2004, via the Wikipedia "Mass-luminosity relation" page, unverified).
double ms_luminosity_solar(double m) {
    if (m < 0.43) {
        return 0.23 * dm::pow(m, 2.3);
    }
    if (m < 2.0) {
        return m * m * m * m;
    }
    if (m < 55.0) {
        return 1.4 * dm::pow(m, 3.5);
    }
    return 32000.0 * m;
}

/// Main-sequence mass-radius relation, R ∝ M^0.8 below 1 M☉ and M^0.57 above (textbook approximation, unverified).
double ms_radius_solar(double m) {
    return m < 1.0 ? dm::pow(m, 0.8) : dm::pow(m, 0.57);
}

/// Main-sequence lifetime ~10 Gyr (M/M☉)^-2.5 (research 03 notes, unverified).
double ms_lifetime_gyr(double m) {
    return 10.0 * dm::pow(m, -2.5);
}

double stefan_boltzmann_temperature(double luminosity_w, double radius_m) {
    if (luminosity_w <= 0.0 || radius_m <= 0.0) {
        return 0.0;
    }
    return std::sqrt(std::sqrt(luminosity_w / (4.0 * si::pi * radius_m * radius_m * si::sigma_sb)));
}

std::string_view spectral_class(double t) {  // Harvard classes by effective temperature
    if (t >= 30000.0) {
        return "O";
    }
    if (t >= 10000.0) {
        return "B";
    }
    if (t >= 7500.0) {
        return "A";
    }
    if (t >= 6000.0) {
        return "F";
    }
    if (t >= 5200.0) {
        return "G";
    }
    if (t >= 3700.0) {
        return "K";
    }
    return "M";
}

StarRecord make_star(HashStream& s) {
    StarRecord star;
    const double m0 = sample_imf_solar(s);
    const double age_gyr = s.uniform(0.05, 10.0);  // constant star formation over the disk's life
    // Local thin disk [Fe/H] ≈ -0.1 ± 0.25 dex (Nordström et al. 2004 give about -0.14 ± 0.19, unverified).
    star.metallicity_feh = std::clamp(-0.1 + 0.25 * gaussian(s), -1.5, 0.5);
    star.initial_mass_kg = m0 * si::solar_mass;
    star.age_s = age_gyr * si::gyr;
    star.zams_luminosity_w = 0.8 * ms_luminosity_solar(m0) * si::solar_luminosity;

    const double t_ms = ms_lifetime_gyr(m0);
    if (age_gyr < t_ms) {
        // The Sun brightens ~30 % over its main sequence: L from 0.8 to 1.2 times the mean relation.
        const double f = age_gyr / t_ms;
        star.kind = star_main_sequence;
        star.mass_kg = star.initial_mass_kg;
        star.luminosity_w = (0.8 + 0.4 * f) * ms_luminosity_solar(m0) * si::solar_luminosity;
        star.radius_m = (0.9 + 0.2 * f) * ms_radius_solar(m0) * si::solar_radius;
        star.temperature_k = stefan_boltzmann_temperature(star.luminosity_w, star.radius_m);
        star.spectral_class = spectral_class(star.temperature_k);
    } else if (age_gyr < 1.1 * t_ms) {
        // Giant branch, 10 % of the main-sequence lifetime: radius 10-100 times, cooling from 4800 K to 3500 K.
        const double f = (age_gyr - t_ms) / (0.1 * t_ms);
        star.kind = star_giant;
        star.mass_kg = star.initial_mass_kg;
        star.radius_m = (10.0 + 90.0 * f) * ms_radius_solar(m0) * si::solar_radius;
        star.temperature_k = 4800.0 - 1300.0 * f;
        const double t2 = star.temperature_k * star.temperature_k;
        star.luminosity_w = 4.0 * si::pi * star.radius_m * star.radius_m * si::sigma_sb * t2 * t2;
        star.spectral_class = spectral_class(star.temperature_k);
    } else if (m0 < 8.0) {
        // White dwarf: initial-final mass relation M_f = 0.109 M_i + 0.394 (Kalirai et al. 2008, unverified);
        // R ∝ M^-1/3 with 0.0127 R☉ at 0.6 M☉; Mestel cooling L ∝ t^-1.4 with 10^-3 L☉ at 1 Gyr (unverified).
        const double mf = 0.109 * m0 + 0.394;
        const double t_cool = std::max(age_gyr - 1.1 * t_ms, 0.01);
        star.kind = star_white_dwarf;
        star.spectral_class = "D";
        star.mass_kg = mf * si::solar_mass;
        star.radius_m = 0.0127 * dm::cbrt(0.6 / mf) * si::solar_radius;
        star.luminosity_w = 1e-3 * dm::pow(t_cool, -1.4) * si::solar_luminosity;
        star.temperature_k = stefan_boltzmann_temperature(star.luminosity_w, star.radius_m);
    } else if (m0 < 20.0) {
        star.kind = star_neutron;
        star.spectral_class = "NS";
        star.mass_kg = 1.4 * si::solar_mass;
        star.radius_m = 12000.0;
        star.luminosity_w = 1e-6 * si::solar_luminosity;
        star.temperature_k = stefan_boltzmann_temperature(star.luminosity_w, star.radius_m);
    } else {
        star.kind = star_black_hole;
        star.spectral_class = "BH";
        star.mass_kg = std::max(5.0, 0.3 * m0) * si::solar_mass;
        star.radius_m = 2.0 * si::G * star.mass_kg / (si::c * si::c);
        star.luminosity_w = 0.0;
        star.temperature_k = 0.0;
    }
    return star;
}

/// Kopparapu et al. 2014 habitable-zone flux limits for 1 Earth mass, S_eff = S☉ + aT + bT² + cT³ + dT⁴ with
/// T = T_eff - 5780 K, valid for 2600-7200 K (clamped). Coefficients from Table 1 as remembered, not re-read from the
/// paper (unverified; the runaway 1.107 and maximum greenhouse 0.356 match research 03's notes).
double kopparapu_seff(double t_eff, bool inner) {
    const double t = std::clamp(t_eff, 2600.0, 7200.0) - 5780.0;
    if (inner) {  // runaway greenhouse
        return 1.107 + t * (1.332e-4 + t * (1.580e-8 + t * (-8.308e-12 + t * -1.931e-15)));
    }
    return 0.356 + t * (6.171e-5 + t * (1.698e-9 + t * (-3.198e-12 + t * -5.575e-16)));  // maximum greenhouse
}

// ---- planets ----

/// Planet radius from mass and composition, in Earth units. Rocky: R = M^0.27 (Zeng et al. 2016, Earth-like,
/// unverified); icy: 1.2 times that (about 50 % water). Envelopes: Chen & Kipping 2017's mean relation, Neptunian R ∝
/// M^0.589 from 2.04 M⊕ (1.23 R⊕) and Jovian R ∝ M^-0.044 above 0.414 M_J (unverified).
double planet_radius_earth(double m_earth, std::string_view comp) {
    if (comp == comp_rocky) {
        return dm::pow(m_earth, 0.27);
    }
    if (comp == comp_icy) {
        return 1.2 * dm::pow(m_earth, 0.27);
    }
    const double r_neptunian = 1.2302 * dm::pow(m_earth / 2.04, 0.589);
    if (m_earth < 131.6) {
        return r_neptunian;
    }
    return 1.2302 * dm::pow(131.6 / 2.04, 0.589) * dm::pow(m_earth / 131.6, -0.044);
}

double sphere_density(double mass, double radius) {
    return mass / (4.0 / 3.0 * si::pi * radius * radius * radius);
}

double orbital_period_s(double a, double central_mass, double mass) {
    return 2.0 * si::pi * std::sqrt(a * a * a / (si::G * (central_mass + mass)));
}

struct Draft {
    double a = 0, e = 0, m = 0;
    std::string_view comp;
};

struct Budgets {
    double inner = 0,
           outer = 0;  ///< kg of solids inside and outside the snow line
};

bool is_giant(std::string_view comp) {
    return comp == comp_gas_giant || comp == comp_ice_giant;
}

double eccentricity(HashStream& s, std::string_view comp) {
    // Rayleigh: σ = 0.03 for small planets in multi-planet systems, 0.05 for
    // giants (Kepler multis are near-circular; a broad reading of Fabrycky et al.
    // 2014 and Van Eylen & Albrecht 2015, unverified).
    return std::min(rayleigh(s, is_giant(comp) ? 0.05 : 0.03), 0.3);
}

/// Draws a planet's composition and mass for its zone and pays its solids from
/// that zone's budget. False when the zone can't pay for even a small body.
bool draw_planet(HashStream& s, bool outer, double star_mass_kg, double m0_solar, double feh, Budgets& budget,
                 Draft& out) {
    constexpr double me = si::earth_mass;
    constexpr double min_body = 0.01 * me;
    double& pool = outer ? budget.outer : budget.inner;
    if (pool < min_body) {
        return false;
    }
    std::string_view comp;
    double m = 0, cost = 0;
    const double u = s.next_double();
    const double g = gaussian(s);
    const double v = s.next_double();
    if (!outer) {
        // Rocky: lognormal, median 1 M⊕ (M*/M☉)^0.5, σ_ln = 1 (a broad fit to
        // Kepler's small-planet population).
        comp = comp_rocky;
        m = std::clamp(std::sqrt(m0_solar) * dm::exp(g), 0.01, 20.0) * me;
        cost = m;
    } else {
        // Gas giants per slot beyond the snow line: 0.07 (M*/M☉) 10^(1.2 [Fe/H]),
        // Johnson et al. 2010's per-star giant-planet occurrence (unverified),
        // applied per slot.
        const double p_gas = std::min(0.9, 0.07 * m0_solar * dm::exp(1.2 * feh * 2.302585092994046));
        if (u < p_gas) {
            // dN/dM ∝ M^-1.3 (dN/dlog M ∝ M^-0.3, Cumming et al. 2008, unverified) on
            // [0.1, 13] M_J, at most 1 % of the star; a 10 M⊕ solid core.
            comp = comp_gas_giant;
            m = std::min(power_law(v, 0.1, 13.0, 1.3) * si::jupiter_mass, 0.01 * star_mass_kg);
            cost = 10.0 * me;
        } else if (u < p_gas + 0.35 * (1.0 - p_gas)) {
            comp = comp_ice_giant;  // 8-25 M⊕, 80 % solids
            m = (8.0 + 17.0 * v) * me;
            cost = 0.8 * m;
        } else {
            comp = comp_icy;  // icy bodies: lognormal, median 1 M⊕
            m = std::clamp(dm::exp(g), 0.01, 10.0) * me;
            cost = m;
        }
    }
    if (cost > pool) {  // what is left becomes one last small body: a failed core
        comp = outer ? comp_icy : comp_rocky;
        m = pool;
        cost = pool;
    }
    pool -= cost;
    out.m = m;
    out.comp = comp;
    return true;
}

/// Outward neighbor: a2 with a2 (1 - e2) - a1 (1 + e1) = K R_H,m. 0 when no
/// position is stable.
double place_outward(const Draft& in, double m2, double e2, double central, double k) {
    const double x = dm::cbrt((in.m + m2) / (3.0 * central));
    const double denom = 1.0 - e2 - 0.5 * k * x;
    return denom < 0.05 ? 0.0 : in.a * (1.0 + in.e + 0.5 * k * x) / denom;
}

/// Inward neighbor: a1 with a2 (1 - e2) - a1 (1 + e1) = K R_H,m. 0 when no
/// position is stable.
double place_inward(const Draft& out, double m1, double e1, double central, double k) {
    const double x = dm::cbrt((m1 + out.m) / (3.0 * central));
    const double num = 1.0 - out.e - 0.5 * k * x;
    return num < 0.05 ? 0.0 : out.a * num / (1.0 + e1 + 0.5 * k * x);
}

double spacing_draw(HashStream& s) {
    // K = 11 + Exp(mean 7): median ≈ 16 mutual Hill radii, the shape of Kepler
    // multis (peak 10-30, Fang & Margot 2013 and Weiss et al. 2018, unverified);
    // one slot in five is left empty (K doubled).
    double k = hill_spacing_generate + exponential(s, 7.0);
    if (s.chance(0.2)) {
        k *= 2.0;
    }
    return k;
}

/// Closest periapsis allowed around the star for a body of this density: 2
/// stellar radii or the fluid Roche limit.
double star_limit(const StarRecord& star, double density) {
    const double star_density = sphere_density(star.mass_kg, star.radius_m);
    return std::max(2.0 * star.radius_m, roche_limit_m(star.radius_m, star_density, density));
}

double draft_density(const Draft& d) {
    const double r = planet_radius_earth(d.m / si::earth_mass, d.comp) * si::earth_radius;
    return sphere_density(d.m, r);
}

/// Tidal locking time (Gladman et al. 1996): t = ω a⁶ I Q / (3 G M² k₂ R⁵), I =
/// α m R². Q = 100, k₂ = 0.3, α = 0.33 for solid bodies; Q = 10^5, k₂ = 0.5, α
/// = 0.25 for giants (unverified textbook values).
bool tidally_locked(double a, double m, double r, double central, double age_s, double p0, bool giant) {
    const double q = giant ? 1e5 : 100.0, k2 = giant ? 0.5 : 0.3, alpha = giant ? 0.25 : 0.33;
    const double omega = 2.0 * si::pi / p0;
    const double a3 = a * a * a;
    const double t = omega * a3 * a3 * alpha * m * q / (3.0 * si::G * central * central * k2 * r * r * r);
    return t < age_s;
}

void finish_body(BodyRecord& b, double central, double age_s, HashStream& spin, HashStream& angles,
                 double inclination_sigma) {
    b.orbit.inclination_rad = rayleigh(angles, inclination_sigma);
    b.orbit.ascending_node_rad = angles.uniform(0.0, 2.0 * si::pi);
    b.orbit.argument_of_periapsis_rad = angles.uniform(0.0, 2.0 * si::pi);
    b.orbit.mean_anomaly_at_epoch_rad = angles.uniform(0.0, 2.0 * si::pi);
    const bool giant = is_giant(b.composition);
    const double p0 =
        giant ? log_uniform(spin, 9.0 * 3600.0, 18.0 * 3600.0) : log_uniform(spin, 8.0 * 3600.0, 48.0 * 3600.0);
    const double obliquity = std::min(rayleigh(spin, 0.4), si::pi);
    b.tidally_locked = tidally_locked(b.orbit.semi_major_axis_m, b.mass_kg, b.radius_m, central, age_s, p0, giant);
    b.sidereal_rotation_s = b.tidally_locked ? orbital_period_s(b.orbit.semi_major_axis_m, central, b.mass_kg) : p0;
    b.obliquity_rad = b.tidally_locked ? 0.0 : obliquity;
}

constexpr std::uint32_t max_planets = 30;  // with at most 6 moons each: 210 bodies, inside the 255 of ADR 0050

void make_moons(const SystemRecord& r, const BodyRecord& planet, std::uint64_t seed, std::vector<BodyRecord>& moons) {
    const std::uint64_t planet_address = r.address.body(planet.index).value;
    HashStream s = HashStream::make(seed, "moons", planet_address);
    const double central = r.star.mass_kg;
    const double hill =
        planet.orbit.semi_major_axis_m * (1.0 - planet.orbit.eccentricity) * dm::cbrt(planet.mass_kg / (3.0 * central));
    // Prograde moons are stable inside 0.49 (1 - 1.03 e_p) R_H (Domingos et al.
    // 2006, unverified).
    const double limit = 0.49 * (1.0 - 1.03 * planet.orbit.eccentricity) * hill;
    std::int64_t n = 0;
    double frac_lo = 0, frac_hi = 0;
    const bool gas = planet.composition == comp_gas_giant, ice = planet.composition == comp_ice_giant;
    // Regular satellites of giants: total ~10^-4 of the planet (Canup & Ward
    // 2006, unverified); one giant-impact moon for a quarter of solid planets,
    // 0.1-2 % of the planet (the Moon is 1.2 %).
    if (gas) {
        n = s.range(0, 6);
        frac_lo = 1e-6;
        frac_hi = 1e-4;
    } else if (ice) {
        n = s.range(0, 5);
        frac_lo = 1e-6;
        frac_hi = 5e-5;
    } else {
        n = s.chance(0.25) ? 1 : 0;
        frac_lo = 1e-3;
        frac_hi = 2e-2;
    }
    Draft prev;
    for (std::int64_t k = 0; k < n; ++k) {
        BodyRecord m;
        m.parent = planet.index;
        m.mass_kg = planet.mass_kg * log_uniform(s, frac_lo, frac_hi);
        m.density_kg_m3 = gas || ice ? s.uniform(1800.0, 3500.0) : (planet.composition == comp_icy ? 1900.0 : 3300.0);
        m.composition = m.density_kg_m3 < 2500.0 ? comp_icy : comp_rocky;
        m.radius_m = dm::cbrt(3.0 * m.mass_kg / (4.0 * si::pi * m.density_kg_m3));
        m.orbit.eccentricity = std::min(rayleigh(s, 0.01), 0.1);
        const double roche =
            std::max(1.5 * planet.radius_m, roche_limit_m(planet.radius_m, planet.density_kg_m3, m.density_kg_m3));
        double a = 0;
        if (k == 0) {
            a = roche * (1.5 + 3.0 * s.next_double()) / (1.0 - m.orbit.eccentricity);
        } else {
            a = place_outward(prev, m.mass_kg, m.orbit.eccentricity, planet.mass_kg, spacing_draw(s));
        }
        if (a <= 0.0 || a * (1.0 + m.orbit.eccentricity) > limit || a * (1.0 - m.orbit.eccentricity) < roche) {
            break;
        }
        m.orbit.semi_major_axis_m = a;
        prev = Draft{a, m.orbit.eccentricity, m.mass_kg, m.composition};
        m.index = static_cast<std::uint32_t>(r.bodies.size() + moons.size() + 1);
        const std::uint64_t moon_address = r.address.body(m.index).value;
        HashStream spin = HashStream::make(seed, "spins", moon_address);
        HashStream angles = HashStream::make(seed, "orbit_angles", moon_address);
        finish_body(m, planet.mass_kg, r.star.age_s, spin, angles, 0.01);
        moons.push_back(m);
    }
}

bool finite(double x) {
    return x == x && x - x == 0.0;
}

/// Walks planets outward and inward from the anchor, alternating, until both
/// directions stop.
struct Walk {
    const StarRecord& star;
    double central, m0, snow, a_in, a_out;
    Budgets& budget;
    HashStream& masses;
    HashStream& orbits;
    std::vector<Draft> inner, outer;

    bool draw(bool outer_zone, Draft& d) {
        return draw_planet(masses, outer_zone, central, m0, star.metallicity_feh, budget, d);
    }

    bool step_out(const Draft& last) {
        const double k = spacing_draw(orbits);
        const bool outer_zone = place_outward(last, last.m, 0.03, central, k) >= snow;
        Draft d;
        bool switched = false;
        if (!draw(outer_zone, d)) {
            if (outer_zone || !draw(true, d)) {
                return false;
            }
            switched = true;  // the rocky budget is spent: continue beyond the snow line
        }
        d.e = eccentricity(orbits, d.comp);
        d.a = place_outward(last, d.m, d.e, central, k);
        if (d.a <= 0.0) {
            return false;
        }
        if (switched) {
            d.a = std::max(d.a, snow);  // only widens the gap
        }
        if (d.a <= 0.0 || d.a * (1.0 + d.e) > a_out) {
            return false;
        }
        outer.push_back(d);
        return true;
    }

    bool step_in(const Draft& last) {
        const double k = spacing_draw(orbits);
        const bool outer_zone = place_inward(last, last.m, 0.03, central, k) >= snow;
        Draft d;
        bool switched = false;
        if (!draw(outer_zone, d)) {
            if (!outer_zone || !draw(false, d)) {
                return false;
            }
            switched = true;  // the icy budget is spent: continue inside the snow line
        }
        d.e = eccentricity(orbits, d.comp);
        d.a = place_inward(last, d.m, d.e, central, k);
        if (d.a <= 0.0) {
            return false;
        }
        if (switched) {
            d.a = std::min(d.a, snow);  // only widens the gap
        }
        if (d.a <= 0.0 || d.a * (1.0 - d.e) < std::max(a_in, star_limit(star, draft_density(d)))) {
            return false;
        }
        inner.push_back(d);
        return true;
    }

    void run(const Draft& anchor) {
        bool out_open = true, in_open = true;
        while ((out_open || in_open) && 1 + inner.size() + outer.size() < max_planets) {
            if (out_open) {
                out_open = step_out(outer.empty() ? anchor : outer.back());
            }
            if (in_open && 1 + inner.size() + outer.size() < max_planets) {
                in_open = step_in(inner.empty() ? anchor : inner.back());
            }
        }
    }
};

}  // namespace

std::uint64_t stage_seed(const StageContext& ctx, std::string_view stage_id, std::uint32_t version) {
    return veach::procgen::stage_seed(ctx.universe->seed, ctx.address.value, stage_id, version);
}

double roche_limit_m(double primary_radius_m, double primary_density, double satellite_density) {
    return 2.44 * primary_radius_m * dm::cbrt(primary_density / satellite_density);
}

double mutual_hill_radius_m(double m1, double m2, double central_mass, double a1, double a2) {
    return dm::cbrt((m1 + m2) / (3.0 * central_mass)) * 0.5 * (a1 + a2);
}

SystemRecord generate_system(const StageContext& ctx) {
    SystemRecord r;
    r.address = ctx.address;
    const std::uint64_t seed = stage_seed(ctx, system_stage_id, system_stage_version);
    const std::uint64_t addr = ctx.address.value;
    constexpr double me = si::earth_mass;

    HashStream star_stream = HashStream::make(seed, "star", addr);
    r.star = make_star(star_stream);
    const StarRecord& star = r.star;
    const double central = star.mass_kg;
    const double m0 = star.initial_mass_kg / si::solar_mass;
    const double l_now = star.luminosity_w / si::solar_luminosity;
    const double l_zams = star.zams_luminosity_w / si::solar_luminosity;

    // Habitable zone: d = sqrt(L / S_eff) AU (Kopparapu et al. 2013/2014).
    if (l_now > 0.0) {
        r.hz_inner_m = std::sqrt(l_now / kopparapu_seff(star.temperature_k, true)) * si::au;
        r.hz_outer_m = std::sqrt(l_now / kopparapu_seff(star.temperature_k, false)) * si::au;
    }
    // Snow line 2.7 AU (L/L☉)^1/2 at formation (Hayashi 1981, unverified).
    r.snow_line_m = 2.7 * std::sqrt(l_zams) * si::au;
    if (star.kind == star_neutron || star.kind == star_black_hole) {
        return r;  // the supernova took the planets (pulsar planets are a later
                   // kind)
    }

    // Disc: inner edge 0.04 AU (M/M☉)^0.6 (TRAPPIST-1's innermost planet is at
    // 0.011 AU, Kepler's at ~0.03-0.05 AU for Sun-like stars), outer 35 AU
    // (M/M☉)^0.5. Giants and white dwarfs engulfed everything inside ~1 AU (M/M☉)
    // (unverified), and mass loss widened the survivors' orbits by M_initial /
    // M_final (adiabatic).
    const double widen = star.initial_mass_kg / central;
    double a_in = 0.04 * dm::pow(m0, 0.6) * si::au;
    if (star.kind == star_giant || star.kind == star_white_dwarf) {
        a_in = std::max(a_in, 1.0 * m0 * widen * si::au);
    }
    const double a_out = 35.0 * std::sqrt(m0) * widen * si::au;
    r.disk_inner_m = a_in;
    r.disk_outer_m = a_out;
    if (a_out <= a_in) {
        return r;
    }

    // Solids: 5 M⊕ inside and 40 M⊕ outside the snow line for a Sun-like star at
    // solar metallicity, linear in M* (disc mass ∝ M*, Andrews et al. 2013,
    // unverified) and in 10^[Fe/H], lognormal scatter. The Solar System has ~2 M⊕
    // of rocky planets and ~50 M⊕ of giant-planet cores (unverified).
    HashStream disk = HashStream::make(seed, "disk", addr);
    const double z = dm::exp(star.metallicity_feh * 2.302585092994046);
    const double inner_scatter = dm::exp(0.8 * gaussian(disk));
    const double outer_scatter = dm::exp(0.5 * gaussian(disk));
    Budgets budget{5.0 * me * m0 * z * inner_scatter, 40.0 * me * m0 * z * outer_scatter};
    const double snow = r.snow_line_m;

    HashStream masses = HashStream::make(seed, "planet_masses", addr);
    HashStream orbits = HashStream::make(seed, "orbits", addr);

    // The anchor: the first planet placed; the rest are walked outward and inward
    // from it. The habitability bias draws it inside the habitable zone as an
    // Earth-sized rocky planet (0.5-3 M⊕), so habitable planets are common (ADR
    // 0066) while every other planet still follows the physics.
    Draft anchor;
    bool have = false;
    const bool biased = disk.chance(ctx.universe->habitability_bias);
    const double hz_lo = std::max(r.hz_inner_m, a_in), hz_hi = std::min(r.hz_outer_m, a_out);
    if (biased && hz_hi > hz_lo) {
        anchor.a = log_uniform(disk, hz_lo, hz_hi);
        anchor.m = log_uniform(masses, 0.5, 3.0) * me;
        anchor.comp = comp_rocky;
        budget.inner = std::max(0.0, budget.inner - anchor.m);
        have = true;
        r.habitability_anchor = true;
    } else {
        anchor.a = log_uniform(disk, a_in, a_out);
        have = draw_planet(masses, anchor.a >= snow, central, m0, star.metallicity_feh, budget, anchor);
    }
    Walk walk{star, central, m0, snow, a_in, a_out, budget, masses, orbits, {}, {}};
    if (have) {
        anchor.e = eccentricity(orbits, anchor.comp);
        const double floor = std::max(a_in, star_limit(star, draft_density(anchor))) / (1.0 - anchor.e);
        anchor.a = std::max(anchor.a, 1.01 * floor);
        walk.run(anchor);
    }

    // Planets in orbit order get body indices 1..P; moons follow, planet by
    // planet.
    std::vector<Draft> planets(walk.inner.rbegin(), walk.inner.rend());
    if (have) {
        planets.push_back(anchor);
    }
    planets.insert(planets.end(), walk.outer.begin(), walk.outer.end());
    r.planet_count = static_cast<std::uint32_t>(planets.size());
    for (const Draft& d : planets) {
        BodyRecord b;
        b.index = static_cast<std::uint32_t>(r.bodies.size() + 1);
        b.parent = 0;
        b.composition = d.comp;
        b.mass_kg = d.m;
        b.radius_m = planet_radius_earth(d.m / me, d.comp) * si::earth_radius;
        b.density_kg_m3 = sphere_density(b.mass_kg, b.radius_m);
        b.orbit.semi_major_axis_m = d.a;
        b.orbit.eccentricity = d.e;
        const std::uint64_t body_address = r.address.body(b.index).value;
        HashStream spin = HashStream::make(seed, "spins", body_address);
        HashStream angles = HashStream::make(seed, "orbit_angles", body_address);
        finish_body(b, central, star.age_s, spin, angles,
                    0.026);  // inclinations: Rayleigh σ = 1.5°
        r.bodies.push_back(b);
    }
    std::vector<BodyRecord> moons;
    for (std::uint32_t i = 0; i < r.planet_count; ++i) {
        make_moons(r, r.bodies[i], seed, moons);
    }
    r.bodies.insert(r.bodies.end(), moons.begin(), moons.end());
    return r;
}

BodyPhysics derive_body_physics(const StageContext& ctx, const SystemRecord& system, std::uint32_t body) {
    const BodyRecord& b = system.bodies[body - 1];
    const BodyRecord& host = b.parent == 0 ? b : system.bodies[b.parent - 1];  // the body that orbits the star
    BodyPhysics p;
    p.index = b.index;
    const std::uint64_t seed = stage_seed(ctx, body_physics_stage_id, body_physics_stage_version);
    HashStream s = HashStream::make(seed, "albedo", system.address.body(b.index).value);
    // Bond albedo: Earth 0.31, Europa ~0.6, Neptune 0.29, Jupiter 0.34
    // (unverified), ± 0.08.
    double albedo = 0.3;
    if (b.composition == comp_icy) {
        albedo = 0.5;
    } else if (b.composition == comp_gas_giant) {
        albedo = 0.34;
    }
    p.bond_albedo = albedo + s.uniform(-0.08, 0.08);
    p.surface_gravity_m_s2 = si::G * b.mass_kg / (b.radius_m * b.radius_m);
    p.escape_velocity_m_s = std::sqrt(2.0 * si::G * b.mass_kg / b.radius_m);
    const double a = host.orbit.semi_major_axis_m, e = host.orbit.eccentricity;
    p.instellation_w_m2 = system.star.luminosity_w / (4.0 * si::pi * a * a * std::sqrt(1.0 - e * e));
    // T_eq = (S (1 - A) / 4σ)^1/4, full redistribution (SpaceEngine's f = 4;
    // tidally locked bodies would use 2).
    p.equilibrium_temperature_k =
        std::sqrt(std::sqrt(p.instellation_w_m2 * (1.0 - p.bond_albedo) / (4.0 * si::sigma_sb)));
    // Jeans: a gas survives Gyr when v_esc ≥ 6 v₀, v₀ = sqrt(2kT/m), at an
    // exobase of 4 T_eq (Earth's is ~1000 K): μ_min = 72 R T / v_esc² (research
    // 03 notes, unverified).
    const double v2 = p.escape_velocity_m_s * p.escape_velocity_m_s;
    p.lightest_retained_molar_mass = 72.0 * si::gas_constant * 4.0 * p.equilibrium_temperature_k / v2;
    // Cosmic shoreline (Zahnle & Catling 2017): I ∝ v_esc⁴, normalized through
    // Mars (5.03 km/s, 0.43 I⊕).
    const double vr = p.escape_velocity_m_s / 5030.0;
    const bool shoreline = p.instellation_w_m2 / solar_constant < 0.43 * vr * vr * vr * vr;
    p.retains_atmosphere = shoreline && p.lightest_retained_molar_mass <= 0.044;
    p.in_habitable_zone = system.hz_outer_m > 0.0 && a >= system.hz_inner_m && a <= system.hz_outer_m;
    const double m_earth = b.mass_kg / si::earth_mass;
    p.potentially_habitable = p.in_habitable_zone && b.composition == comp_rocky && m_earth >= 0.1 && m_earth <= 5.0 &&
                              p.retains_atmosphere && p.lightest_retained_molar_mass <= 0.028;
    // SpaceEngine's temperature classes (upper limits 90, 170, 250, 330, 500,
    // 1000 K).
    const double t = p.equilibrium_temperature_k;
    if (t < 90.0) {
        p.temperature_class = "frigid";
    } else if (t < 170.0) {
        p.temperature_class = "cold";
    } else if (t < 250.0) {
        p.temperature_class = "cool";
    } else if (t < 330.0) {
        p.temperature_class = "temperate";
    } else if (t < 500.0) {
        p.temperature_class = "warm";
    } else if (t < 1000.0) {
        p.temperature_class = "hot";
    } else {
        p.temperature_class = "torrid";
    }
    return p;
}

std::vector<BodyPhysics> derive_all_body_physics(const StageContext& ctx, const SystemRecord& system) {
    std::vector<BodyPhysics> out;
    out.reserve(system.bodies.size());
    for (const BodyRecord& b : system.bodies) {
        out.push_back(derive_body_physics(ctx, system, b.index));
    }
    return out;
}

std::uint64_t stage_hash(const SystemRecord& r) {
    veach::procgen::RecordHasher h(system_stage_id, system_stage_version);
    h.add(r.address.value);
    const StarRecord& s = r.star;
    h.add_id(s.kind);
    h.add_id(s.spectral_class);
    for (const double x :
         {s.initial_mass_kg, s.mass_kg, s.age_s, s.metallicity_feh, s.zams_luminosity_w, s.luminosity_w, s.radius_m,
          s.temperature_k, r.disk_inner_m, r.disk_outer_m, r.snow_line_m, r.hz_inner_m, r.hz_outer_m}) {
        h.add_double(x);
    }
    h.add(r.habitability_anchor ? 1u : 0u);
    h.add(r.planet_count);
    h.add(r.bodies.size());
    for (const BodyRecord& b : r.bodies) {
        h.add(b.index);
        h.add(b.parent);
        h.add_id(b.composition);
        for (const double x : {b.mass_kg, b.radius_m, b.density_kg_m3, b.orbit.semi_major_axis_m, b.orbit.eccentricity,
                               b.orbit.inclination_rad, b.orbit.ascending_node_rad, b.orbit.argument_of_periapsis_rad,
                               b.orbit.mean_anomaly_at_epoch_rad, b.sidereal_rotation_s, b.obliquity_rad}) {
            h.add_double(x);
        }
        h.add(b.tidally_locked ? 1u : 0u);
    }
    return h.value();
}

std::uint64_t stage_hash(const std::vector<BodyPhysics>& r) {
    veach::procgen::RecordHasher h(body_physics_stage_id, body_physics_stage_version);
    h.add(r.size());
    for (const BodyPhysics& p : r) {
        h.add(p.index);
        for (const double x : {p.bond_albedo, p.surface_gravity_m_s2, p.escape_velocity_m_s, p.instellation_w_m2,
                               p.equilibrium_temperature_k, p.lightest_retained_molar_mass}) {
            h.add_double(x);
        }
        h.add((p.retains_atmosphere ? 1u : 0u) | (p.in_habitable_zone ? 2u : 0u) | (p.potentially_habitable ? 4u : 0u));
        h.add_id(p.temperature_class);
    }
    return h.value();
}

Violations& Violations::operator+=(const Violations& o) {
    planet_hill += o.planet_hill;
    planet_star += o.planet_star;
    moon_roche += o.moon_roche;
    moon_hill += o.moon_hill;
    moon_spacing += o.moon_spacing;
    body_overflow += o.body_overflow;
    non_finite += o.non_finite;
    return *this;
}

Violations validate(const SystemRecord& r, const std::vector<BodyPhysics>& physics) {
    Violations v;
    const double central = r.star.mass_kg;
    const BodyRecord* prev_planet = nullptr;
    for (std::uint32_t i = 0; i < r.planet_count; ++i) {
        const BodyRecord& b = r.bodies[i];
        const double peri = b.orbit.semi_major_axis_m * (1.0 - b.orbit.eccentricity);
        if (peri < star_limit(r.star, b.density_kg_m3)) {
            ++v.planet_star;
        }
        if (prev_planet != nullptr) {
            const BodyRecord& a = *prev_planet;
            const double gap = peri - a.orbit.semi_major_axis_m * (1.0 + a.orbit.eccentricity);
            const double rh = mutual_hill_radius_m(a.mass_kg, b.mass_kg, central, a.orbit.semi_major_axis_m,
                                                   b.orbit.semi_major_axis_m);
            if (gap < hill_spacing_required * rh) {
                ++v.planet_hill;
            }
        }
        prev_planet = &b;
    }
    const BodyRecord* prev_moon = nullptr;
    for (std::size_t i = r.planet_count; i < r.bodies.size(); ++i) {
        const BodyRecord& m = r.bodies[i];
        const BodyRecord& p = r.bodies[m.parent - 1];
        const double peri = m.orbit.semi_major_axis_m * (1.0 - m.orbit.eccentricity);
        const double apo = m.orbit.semi_major_axis_m * (1.0 + m.orbit.eccentricity);
        if (peri < roche_limit_m(p.radius_m, p.density_kg_m3, m.density_kg_m3)) {
            ++v.moon_roche;
        }
        const double hill =
            p.orbit.semi_major_axis_m * (1.0 - p.orbit.eccentricity) * dm::cbrt(p.mass_kg / (3.0 * central));
        if (apo > 0.49 * (1.0 - 1.03 * p.orbit.eccentricity) * hill) {
            ++v.moon_hill;
        }
        if (prev_moon != nullptr && prev_moon->parent == m.parent) {
            const double gap = peri - prev_moon->orbit.semi_major_axis_m * (1.0 + prev_moon->orbit.eccentricity);
            const double rh = mutual_hill_radius_m(prev_moon->mass_kg, m.mass_kg, p.mass_kg,
                                                   prev_moon->orbit.semi_major_axis_m, m.orbit.semi_major_axis_m);
            if (gap < hill_spacing_required * rh) {
                ++v.moon_spacing;
            }
        }
        prev_moon = &m;
    }
    if (r.bodies.size() > max_generated_body) {
        ++v.body_overflow;
    }
    const StarRecord& s = r.star;
    for (const double x : {s.mass_kg, s.luminosity_w, s.radius_m, s.temperature_k, r.snow_line_m, r.hz_inner_m,
                           r.hz_outer_m, r.disk_inner_m, r.disk_outer_m}) {
        v.non_finite += finite(x) ? 0u : 1u;
    }
    for (const BodyRecord& b : r.bodies) {
        for (const double x : {b.mass_kg, b.radius_m, b.density_kg_m3, b.orbit.semi_major_axis_m, b.orbit.eccentricity,
                               b.orbit.inclination_rad, b.sidereal_rotation_s, b.obliquity_rad}) {
            v.non_finite += finite(x) ? 0u : 1u;
        }
    }
    for (const BodyPhysics& p : physics) {
        for (const double x : {p.surface_gravity_m_s2, p.escape_velocity_m_s, p.instellation_w_m2,
                               p.equilibrium_temperature_k, p.lightest_retained_molar_mass}) {
            v.non_finite += finite(x) ? 0u : 1u;
        }
    }
    return v;
}

}  // namespace space::gen
