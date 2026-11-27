#pragma once

// The realization layer of docs/adr/0049-scale-model.md, spike SP-05 (VCH-081), throwaway: the only code that takes
// a scale factor. It turns physical records into gameplay geometry; it never writes back into a record.

#include <veach/space/gen/system.hpp>

#include <cstdint>
#include <vector>

namespace space::gen {

struct ScaleFactors {
    double s_p = 0.1;  ///< Body radii, moon orbits
    double s_o = 0.1;  ///< Planet orbits, star radius (ADR 0049 default: s_o = s_p)
};

struct RealizedBody {
    std::uint32_t index = 0;
    double radius_m = 0;
    double orbit_radius_m = 0;  ///< Semi-major axis around its parent
    double gm_m3_s2 = 0;        ///< Gameplay GM = g R² (surface gravity kept physical, ADR 0049)
};

struct RealizedSystem {
    double star_radius_m = 0;
    double star_gm_m3_s2 = 0;
    std::vector<RealizedBody> bodies;
};

RealizedSystem realize(const SystemRecord& record, const std::vector<BodyPhysics>& physics, const ScaleFactors& s);
std::uint64_t realized_hash(const RealizedSystem& r);

/// The same Hill and Roche checks as validate(), on realized geometry and gameplay GM.
struct RealizedViolations {
    std::uint64_t moon_roche = 0;
    std::uint64_t moon_hill = 0;
};
RealizedViolations validate_realized(const SystemRecord& record, const RealizedSystem& r);

}  // namespace space::gen
