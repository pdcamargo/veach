#pragma once

// The sun from the system (docs/adr/0048-bodies-on-rails.md): the star's direction for an observer on a rotating,
// tilted body, from the orbit tree, instead of Earth astronomy. Spike SP-04 (VCH-080).
//
// The local frame is east, north, up at the observer's latitude and longitude on the body's surface. The direction
// is canonical (dmath and + − × ÷ √); the angles (elevation, azimuth, local solar time) use asin and atan2 and are
// for display and tests (ADR 0043: no asin or atan2 in the authoritative path).

#include <veach/space/orbit.hpp>

#include <cstdint>

namespace veach::space {

struct SunSample {
    math::dvec3 local{0.0};         ///< unit direction to the star's center: (east, north, up)
    double distance = 0.0;          ///< m, observer to the star's center
    double elevation = 0.0;         ///< radians above the horizon
    double azimuth = 0.0;           ///< radians from north toward east
    double local_solar_time = 0.0;  ///< hours in [0, 24): 12 when the star crosses the observer's meridian
};

/// The star `star` seen from latitude/longitude (radians) on the surface of `body` at `tick`.
[[nodiscard]] SunSample sun_local(const System& system, std::int32_t body, std::int32_t star, double latitude,
                                  double longitude, std::int64_t tick);

/// The body-fixed point under the star (from the body's center): latitude and longitude, radians.
struct SubsolarPoint {
    double latitude = 0.0;
    double longitude = 0.0;
};
[[nodiscard]] SubsolarPoint subsolar_point(const System& system, std::int32_t body, std::int32_t star,
                                           std::int64_t tick);

/// The observer's position on a unit sphere and its east, north and up axes, in body-fixed coordinates
/// (X: prime meridian, Y: north pole, longitude increasing eastward, i.e. with the rotation).
struct LocalFrame {
    math::dvec3 east, north, up;
};
[[nodiscard]] LocalFrame local_frame(double latitude, double longitude);

}  // namespace veach::space
