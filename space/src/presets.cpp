// veach: canonical (ADR 0043): building a system uses only + − × ÷ √ and dmath, so its elements are the same bits
// everywhere.
#include <veach/space/presets.hpp>

#include <veach/space/clock.hpp>

#include <array>
#include <cmath>

namespace veach::space {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double deg = pi / 180.0;
constexpr double day = 86'400.0;
constexpr double julian_year = 365.25 * day;
constexpr double century = 36'525.0 * day;

/// One row pair of JPL Table 1: values at J2000 and rates per Julian century. Degrees and au as published.
struct Table1Row {
    const char* name;
    double a, e, i, mean_longitude, longitude_perihelion, node;
    double a_dot, e_dot, i_dot, mean_longitude_dot, longitude_perihelion_dot, node_dot;
    double radius_km, gm;  // NASA planetary fact sheet: volumetric mean radius; GM, m³/s²
};

// JPL Solar System Dynamics, "Approximate Positions of the Planets", Table 1: Keplerian elements and their rates
// with respect to the mean ecliptic and equinox of J2000, valid for 1800 AD - 2050 AD (E. M. Standish).
// https://ssd.jpl.nasa.gov/planets/approx_pos.html, page fetched 2026-11-27 (SHA-256 of the HTML: 9f7b30ca...d742).
// Copied digit for digit; "EM Bary" is the Earth-Moon barycenter.
constexpr Table1Row table1[] = {
    {"mercury", 0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593,  //
     0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081, 2439.7, 2.2032e13},
    {"venus", 0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255,  //
     0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418, 6051.8, 3.24859e14},
    {"emb", 1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,  //
     0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0, 0.0, 0.0},
    {"mars", 1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891,  //
     0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343, 3389.5, 4.282837e13},
    {"jupiter", 5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,  //
     -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106, 69911.0, 1.26686534e17},
    {"saturn", 9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448,  //
     -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794, 58232.0, 3.7931187e16},
    {"uranus", 19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503,  //
     -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589, 25362.0, 5.793939e15},
    {"neptune", 30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574,  //
     0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664, 24622.0, 6.836529e15},
};

// The Moon: JPL SSD "Planetary Satellite Mean Elements" (https://ssd.jpl.nasa.gov/sats/elem/), Earth, mean ecliptic
// elements, epoch 2000-01-01.5 TDB (DE405/LE405): a 384400 km, e 0.0554, ω 318.15°, M 135.27°, i 5.16°,
// node 125.08°, P 27.322 d (sidereal), P apsis 5.997 yr (of ω), P node 18.600 yr (regressing).
constexpr double moon_a_km = 384'400.0, moon_e = 0.0554, moon_w = 318.15, moon_m = 135.27, moon_i = 5.16,
                 moon_node = 125.08, moon_period_days = 27.322, moon_apsis_years = 5.997, moon_node_years = 18.600;
constexpr double earth_moon_mass_ratio = 81.30056;  // DE430
constexpr double gm_earth = 3.986004418e14, gm_moon = 4.9028e12;
constexpr double earth_obliquity_deg = 23.43928;       // JPL's ε at J2000
constexpr double earth_gmst_j2000_deg = 280.46061837;  // GMST at J2000.0 (IAU 1982), approximate: TDB taken as UT1

OrbitNode planet_node(const Table1Row& row, std::int32_t parent, ScaleFactors s) {
    const double time_scale = std::sqrt(s.orbit);  // every period × √s_o
    OrbitNode n;
    n.name = row.name;
    n.parent = parent;
    n.epoch_tick = sol_j2000_tick;
    n.elements = {.a = row.a * au * s.orbit,
                  .e = row.e,
                  .i = row.i * deg,
                  .node = row.node * deg,
                  .arg_periapsis = (row.longitude_perihelion - row.node) * deg,
                  .mean_anomaly = row.mean_longitude * deg};
    n.rates = {.a = row.a_dot * au * s.orbit,
               .e = row.e_dot,
               .i = row.i_dot * deg,
               .node = row.node_dot * deg,
               .longitude_periapsis = row.longitude_perihelion_dot * deg,
               .t_min = -2.0,  // 1800-01-01.5
               .t_max = 0.5,   // 2050-01-01.5
               .century_seconds = century * time_scale};
    n.period_q = period_q_from_seconds(360.0 / row.mean_longitude_dot * century * time_scale);
    n.gm = row.gm * s.body * s.body;
    n.radius = row.radius_km * 1000.0 * s.body;
    return n;
}

}  // namespace

std::int64_t sol_tick_from_jd(double jd_tdb) {
    return sol_j2000_tick + std::llround((jd_tdb - 2'451'545.0) * static_cast<double>(ticks_per_day));
}

double orbital_period(double a, double gm) {
    return 2.0 * pi * std::sqrt(a * a * a / gm);
}

System sol(ScaleFactors s) {
    System sys("sol");
    OrbitNode sun;
    sun.name = "sun";
    sun.gm = gm_sun * s.orbit * s.orbit;
    sun.radius = 695'700'000.0 * s.orbit;
    const std::int32_t root = sys.add(sun, "space:fixed");

    for (const Table1Row& row : table1) {
        const std::int32_t index = sys.add(planet_node(row, root, s), "space:kepler_secular");
        if (std::string_view(row.name) != "emb") {
            continue;
        }
        // The Moon's orbit relative to the Earth, shared by both children of the barycenter.
        const double month_scale = std::sqrt(s.body);
        OrbitNode moon;
        moon.parent = index;
        moon.epoch_tick = sol_j2000_tick;
        moon.elements = {.a = moon_a_km * 1000.0 * s.body,
                         .e = moon_e,
                         .i = moon_i * deg,
                         .node = moon_node * deg,
                         .arg_periapsis = moon_w * deg,
                         .mean_anomaly = (moon_node + moon_w + moon_m) * deg};         // mean longitude
        const double node_rate = -360.0 / (moon_node_years * julian_year) * century;   // degrees per century
        const double apsis_rate = 360.0 / (moon_apsis_years * julian_year) * century;  // of ω
        moon.rates = {.node = node_rate * deg,
                      .longitude_periapsis = (node_rate + apsis_rate) * deg,
                      .t_min = -1e9,
                      .t_max = 1e9,
                      .century_seconds = century * month_scale};
        moon.period_q = period_q_from_seconds(moon_period_days * day * month_scale);
        const double mu = 1.0 / (1.0 + earth_moon_mass_ratio);

        OrbitNode earth = moon;
        earth.name = "earth";
        earth.mass_fraction = -mu;
        earth.gm = gm_earth * s.body * s.body;
        earth.radius = 6'371'000.0 * s.body;
        const double year = seconds_from_period_q(sys.node(index).period_q);
        earth.spin = {.period_q = period_q_from_seconds(sidereal_from_solar(day, year)),
                      .epoch_tick = sol_j2000_tick,
                      // The body's X axis faces right ascension 90° at angle 0 (pole longitude 90°).
                      .phase0 = (earth_gmst_j2000_deg - 90.0) * deg,
                      .obliquity = earth_obliquity_deg * deg,
                      .pole_longitude = 90.0 * deg};
        sys.add(earth, "space:kepler_secular");

        moon.name = "moon";
        moon.mass_fraction = 1.0 - mu;
        moon.gm = gm_moon * s.body * s.body;
        moon.radius = 1'737'400.0 * s.body;
        sys.add(moon, "space:kepler_secular");
    }
    return sys;
}

System earthlike(ScaleFactors s) {
    System sys("earthlike");
    OrbitNode sun;
    sun.name = "sun";
    sun.gm = gm_sun * s.orbit * s.orbit;
    sun.radius = 695'700'000.0 * s.orbit;
    const std::int32_t root = sys.add(sun, "space:fixed");

    OrbitNode planet;
    planet.name = "planet";
    planet.parent = root;
    // At tick 0 the planet is at orbital longitude 270° (system +Z), so the sun is toward −Z, where the pole tilts:
    // the June solstice.
    planet.elements = {.a = au * s.orbit, .mean_anomaly = 1.5 * pi};
    const double year = orbital_period(planet.elements.a, sun.gm);
    planet.period_q = period_q_from_seconds(year);
    planet.gm = gm_earth * s.body * s.body;
    planet.radius = 6'371'000.0 * s.body;
    planet.spin = {.period_q = period_q_from_seconds(sidereal_from_solar(day, year)),
                   .phase0 = 0.0,  // the prime meridian faces the sun at tick 0
                   .obliquity = 23.4 * deg,
                   .pole_longitude = 90.0 * deg};
    sys.add(planet, "space:kepler_elliptic");
    return sys;
}

System reference_system() {
    System sys("reference");
    OrbitNode root;
    root.name = "barycenter";
    const std::int32_t bary = sys.add(root, "space:fixed");

    const auto orbit = [](const char* name, std::int32_t parent, Elements el, double gm_parent) {
        OrbitNode n;
        n.name = name;
        n.parent = parent;
        n.elements = el;
        n.period_q = period_q_from_seconds(orbital_period(el.a, gm_parent));
        return n;
    };

    // Binary star: B (0.6 solar masses) relative to A (1.0), split about the barycenter.
    const double gm_a = gm_sun, gm_b = 0.6 * gm_sun, gm_ab = gm_a + gm_b;
    const Elements binary{.a = 0.3 * au, .e = 0.4, .i = 0.0, .node = 0.0, .arg_periapsis = 0.5, .mean_anomaly = 0.0};
    OrbitNode a = orbit("star_a", bary, binary, gm_ab);
    a.mass_fraction = -gm_b / gm_ab;
    a.gm = gm_a;
    a.radius = 6.957e8;
    const std::int32_t star_a = sys.add(a, "space:kepler_elliptic");
    OrbitNode b = orbit("star_b", bary, binary, gm_ab);
    b.mass_fraction = gm_a / gm_ab;
    b.gm = gm_b;
    b.radius = 4.5e8;
    sys.add(b, "space:kepler_elliptic");

    // A circumbinary planet with a 30-hour solar day.
    OrbitNode p = orbit("circumbinary", bary,
                        {.a = 2.5 * au,
                         .e = 0.05,
                         .i = 2.0 * deg,
                         .node = 40.0 * deg,
                         .arg_periapsis = 10.0 * deg,
                         .mean_anomaly = 1.0},
                        gm_ab);
    p.radius = 5.0e6;
    p.spin = {.period_q = period_q_from_seconds(sidereal_from_solar(30.0 * 3600.0, seconds_from_period_q(p.period_q))),
              .phase0 = 0.3,
              .obliquity = 10.0 * deg,
              .pole_longitude = 200.0 * deg};
    sys.add(p, "space:kepler_elliptic");

    // An S-type giant around A, its moon (tidally locked) and the moon's moon.
    const double gm_giant = 1.26686534e17, gm_moon_r = 5.0e12;
    OrbitNode g = orbit(
        "giant", star_a,
        {.a = 0.06 * au, .e = 0.1, .i = 1.0 * deg, .node = 5.0 * deg, .arg_periapsis = 2.0, .mean_anomaly = 4.0}, gm_a);
    g.radius = 7.0e7;
    const std::int32_t giant = sys.add(g, "space:kepler_elliptic");
    OrbitNode m = orbit("giant_moon", giant,
                        {.a = 4.0e8, .e = 0.02, .i = 5.0 * deg, .node = 1.0, .arg_periapsis = 0.2, .mean_anomaly = 3.0},
                        gm_giant);
    m.radius = 1.8e6;
    m.spin = {.period_q = m.period_q, .phase0 = 0.0, .obliquity = 0.0, .pole_longitude = 0.0};  // tidally locked
    const std::int32_t moon = sys.add(m, "space:kepler_elliptic");
    sys.add(orbit("giant_moon_moon", moon,
                  {.a = 8.0e6, .e = 0.01, .i = 20.0 * deg, .node = 2.0, .arg_periapsis = 1.0, .mean_anomaly = 0.5},
                  gm_moon_r),
            "space:kepler_elliptic");

    // Comets around the barycenter, up to e = 0.9999, and a period over 2^47 period_q units (25,000 years).
    sys.add(orbit("comet_095", bary,
                  {.a = 3.0 * au, .e = 0.95, .i = 30.0 * deg, .node = 1.0, .arg_periapsis = 2.0, .mean_anomaly = 0.1},
                  gm_ab),
            "space:kepler_elliptic");
    sys.add(
        orbit("comet_099", bary,
              {.a = 1000.0 * au, .e = 0.99, .i = 100.0 * deg, .node = 3.0, .arg_periapsis = 0.7, .mean_anomaly = 6.0},
              gm_ab),
        "space:kepler_elliptic");
    sys.add(
        orbit("comet_09999", bary,
              {.a = 30.0 * au, .e = 0.9999, .i = 60.0 * deg, .node = 4.0, .arg_periapsis = 5.0, .mean_anomaly = 1e-6},
              gm_ab),
        "space:kepler_elliptic");
    return sys;
}

std::span<const std::string_view> preset_names() {
    static constexpr std::array<std::string_view, 3> names{"sol", "earthlike", "reference"};
    return names;
}

std::optional<System> preset(std::string_view name, double scale) {
    const ScaleFactors s{.body = scale, .orbit = scale};
    if (name == "sol") {
        return sol(s);
    }
    if (name == "earthlike") {
        return earthlike(s);
    }
    if (name == "reference") {
        return reference_system();
    }
    return std::nullopt;
}

std::int32_t primary_star(const System& system) {
    const std::int32_t sun = system.find("sun");
    const std::int32_t star_a = system.find("star_a");
    return sun >= 0 ? sun : (star_a >= 0 ? star_a : 0);
}

}  // namespace veach::space
