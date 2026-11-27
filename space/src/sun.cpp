// veach: canonical for the direction (dmath and + − × ÷ √); the angles use asin/atan2 for display.
#include <veach/space/sun.hpp>

#include <veach/core/dmath.hpp>

#include <algorithm>
#include <cmath>

namespace veach::space {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;

math::dvec3 unit(math::dvec3 v) {
    return v / std::sqrt(math::dot(v, v));
}

/// The longitude of a body-fixed direction: x = cos λ, z = −sin λ.
double longitude_of(math::dvec3 v) {
    return dmath::atan2(-v.z, v.x);
}

}  // namespace

LocalFrame local_frame(double latitude, double longitude) {
    const double cl = dmath::cos(latitude), sl = dmath::sin(latitude);
    const double co = dmath::cos(longitude), so = dmath::sin(longitude);
    return {
        .east = {-so, 0.0, -co},
        .north = {-sl * co, cl, sl * so},
        .up = {cl * co, sl, -cl * so},
    };
}

SunSample sun_local(const System& system, std::int32_t body, std::int32_t star, double latitude, double longitude,
                    std::int64_t tick) {
    const OrbitNode& b = system.node(body);
    const Orientation r = orientation(b.spin, tick);
    const LocalFrame f = local_frame(latitude, longitude);
    const math::dvec3 observer = system.evaluate(body, tick).position + r.to_system(f.up * b.radius);
    const math::dvec3 to_star = system.evaluate(star, tick).position - observer;
    const double distance = std::sqrt(math::dot(to_star, to_star));
    const math::dvec3 d = r.to_body(to_star / distance);

    SunSample s;
    s.local = {math::dot(d, f.east), math::dot(d, f.north), math::dot(d, f.up)};
    s.distance = distance;
    s.elevation = dmath::asin(std::clamp(s.local.z, -1.0, 1.0));
    s.azimuth = dmath::atan2(s.local.x, s.local.y);
    // Hour angle: how far west of the observer's meridian the star's meridian is.
    double hour_angle = longitude - longitude_of(d);
    hour_angle -= 2.0 * pi * std::floor((hour_angle + pi) / (2.0 * pi));
    s.local_solar_time = 12.0 + hour_angle * (12.0 / pi);
    return s;
}

SubsolarPoint subsolar_point(const System& system, std::int32_t body, std::int32_t star, std::int64_t tick) {
    const Orientation r = orientation(system.node(body).spin, tick);
    const math::dvec3 d = r.to_body(unit(system.evaluate(star, tick).position - system.evaluate(body, tick).position));
    return {dmath::asin(std::clamp(d.y, -1.0, 1.0)), longitude_of(d)};
}

}  // namespace veach::space
