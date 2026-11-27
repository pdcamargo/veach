#include <veach/space/gen/realize.hpp>

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>

#include <bit>

namespace space::gen {

namespace dm = veach::dmath;

RealizedSystem realize(const SystemRecord& record, const std::vector<BodyPhysics>& physics, const ScaleFactors& s) {
    RealizedSystem out;
    // Stars follow s_o; gameplay GM = g R², so it scales with the square of the radius factor (ADR 0049).
    out.star_radius_m = record.star.radius_m * s.s_o;
    out.star_gm_m3_s2 = si::G * record.star.mass_kg * s.s_o * s.s_o;
    out.bodies.reserve(record.bodies.size());
    for (const BodyRecord& b : record.bodies) {
        RealizedBody rb;
        rb.index = b.index;
        rb.radius_m = b.radius_m * s.s_p;
        rb.orbit_radius_m = b.orbit.semi_major_axis_m * (b.parent == 0 ? s.s_o : s.s_p);
        rb.gm_m3_s2 = physics[b.index - 1].surface_gravity_m_s2 * rb.radius_m * rb.radius_m;
        out.bodies.push_back(rb);
    }
    return out;
}

std::uint64_t realized_hash(const RealizedSystem& r) {
    std::uint64_t h = veach::hash_id("realized");
    const auto add = [&h](double x) {
        h = veach::hash(h, std::bit_cast<std::uint64_t>(x));
    };
    add(r.star_radius_m);
    add(r.star_gm_m3_s2);
    for (const RealizedBody& b : r.bodies) {
        h = veach::hash(h, std::uint64_t{b.index});
        add(b.radius_m);
        add(b.orbit_radius_m);
        add(b.gm_m3_s2);
    }
    return h;
}

RealizedViolations validate_realized(const SystemRecord& record, const RealizedSystem& r) {
    RealizedViolations v;
    for (std::size_t i = record.planet_count; i < record.bodies.size(); ++i) {
        const BodyRecord& m = record.bodies[i];
        const RealizedBody& rm = r.bodies[i];
        const BodyRecord& p = record.bodies[m.parent - 1];
        const RealizedBody& rp = r.bodies[m.parent - 1];
        // Gameplay densities from gameplay GM: ρ = 3 GM / (4π G R³). Both bodies scale alike, so Roche is invariant.
        const double rho_p = 3.0 * rp.gm_m3_s2 / (4.0 * si::pi * si::G * rp.radius_m * rp.radius_m * rp.radius_m);
        const double rho_m = 3.0 * rm.gm_m3_s2 / (4.0 * si::pi * si::G * rm.radius_m * rm.radius_m * rm.radius_m);
        if (rm.orbit_radius_m * (1.0 - m.orbit.eccentricity) < roche_limit_m(rp.radius_m, rho_p, rho_m)) {
            ++v.moon_roche;
        }
        const double hill =
            rp.orbit_radius_m * (1.0 - p.orbit.eccentricity) * dm::cbrt(rp.gm_m3_s2 / (3.0 * r.star_gm_m3_s2));
        if (rm.orbit_radius_m * (1.0 + m.orbit.eccentricity) > 0.49 * (1.0 - 1.03 * p.orbit.eccentricity) * hill) {
            ++v.moon_hill;
        }
    }
    return v;
}

}  // namespace space::gen
