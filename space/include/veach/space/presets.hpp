#pragma once

// Star systems for the spike SP-04 (VCH-080): Sol from published elements, an Earth-like test planet, and a reference
// system for the cross-platform orbit hash. Each is realized at a scale (docs/adr/0049-scale-model.md): body radii and
// moon orbits × s_p, star radii and planet orbits × s_o, every GM × s², so every period × √s.

#include <veach/space/clock.hpp>
#include <veach/space/orbit.hpp>

#include <cstdint>

namespace veach::space {

struct ScaleFactors {
    double body = 1.0;   ///< s_p
    double orbit = 1.0;  ///< s_o
};

inline constexpr double au = 149'597'870'700.0;     ///< m (IAU 2012)
inline constexpr double gm_sun = 1.32712440018e20;  ///< m³/s²

/// Sol's universe epoch is 1800-01-01 00:00 TDB (JD 2378496.5), the start of JPL Table 1's interval, so the whole
/// interval has non-negative ticks. J2000.0 (JD 2451545.0 TDB), the elements' epoch, is this tick.
inline constexpr std::int64_t sol_j2000_tick = 73'048 * ticks_per_day + ticks_per_day / 2;
/// The tick of a Julian date (TDB) in Sol, rounded to a whole tick.
[[nodiscard]] std::int64_t sol_tick_from_jd(double jd_tdb);

/// The Sun, the eight planets and the Earth-Moon barycenter with the Earth and the Moon split by mass ratio.
/// Planets: JPL "Approximate Positions of the Planets" (E. M. Standish), Table 1 (1800-2050), as `kepler_secular`.
/// The Moon: JPL's mean ecliptic elements at J2000 (ssd.jpl.nasa.gov/sats/elem/) with their apsidal and nodal
/// periods. The Earth spins (obliquity 23.43928°, the prime meridian at GMST(J2000)). Node names: "sun",
/// "mercury", "venus", "emb", "earth", "moon", "mars", "jupiter", "saturn", "uranus", "neptune".
[[nodiscard]] System sol(ScaleFactors scale = {});

/// "sun" and "planet": a circular orbit at 1 AU × s_o, the planet 6371 km × s_p in radius, tilted 23.4° and turning
/// once per 24-hour solar day at every scale. The June solstice is at tick 0 with the sun on the prime meridian;
/// the September equinox a quarter of the year later.
[[nodiscard]] System earthlike(ScaleFactors scale = {});

/// For the orbit hash: a binary star, a circumbinary (P-type) planet, an S-type giant with a moon that has a moon,
/// comets at e = 0.95, 0.99 and 0.9999, spins (one tidally locked). `kepler_elliptic` only.
[[nodiscard]] System reference_system();

/// 2π √(a³ / GM), seconds.
[[nodiscard]] double orbital_period(double a, double gm);

}  // namespace veach::space
