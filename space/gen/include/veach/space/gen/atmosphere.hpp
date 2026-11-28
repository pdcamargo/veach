#pragma once

// Stage `space:body.atmosphere` (VCH-115): what each body's air is made of, from its B1 record. Physical and
// unscaled, like every stage (ADR 0049: no stage reads s); the realization layer turns it into a render record
// (veach::space::atmosphere_from, scaled by s_a). Canonical: dmath, the stage's hash streams and + − × ÷ √ only.
//
// ADR 0051 lists "atmosphere" under B1, but B1 (`space:body.physics`) is frozen at version 1 with SP-05's committed
// hashes, so the composition is a stage of its own that reads B1 (ADR 0088). The rules are a first, deliberately
// small classifier, not climate science: temperate N2/O2 air on potentially habitable bodies, N2/CH4 haze on cold
// ones that keep methane, CO2 on the rest that keep an atmosphere, none on bodies that don't (or have no surface).

#include <veach/procgen/stage.hpp>
#include <veach/space/gen/system.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace space::gen {

inline constexpr std::string_view atmosphere_stage_id = "space:body.atmosphere";
inline constexpr std::uint32_t atmosphere_stage_version = 1;

/// Gas ids: the names veach::space::rayleigh_cross_section knows.
inline constexpr std::string_view gas_n2 = "N2";
inline constexpr std::string_view gas_o2 = "O2";
inline constexpr std::string_view gas_ar = "Ar";
inline constexpr std::string_view gas_co2 = "CO2";
inline constexpr std::string_view gas_ch4 = "CH4";
inline constexpr std::string_view gas_h2 = "H2";

/// Atmosphere kinds (stable ids, ADR 0051).
inline constexpr std::string_view atmosphere_none = "space:atmosphere.none";
inline constexpr std::string_view atmosphere_temperate = "space:atmosphere.temperate";
inline constexpr std::string_view atmosphere_hazy = "space:atmosphere.hazy";
inline constexpr std::string_view atmosphere_carbon_dioxide = "space:atmosphere.carbon_dioxide";

/// Aerosol kinds.
inline constexpr std::string_view aerosol_clear = "space:aerosol.clear";
inline constexpr std::string_view aerosol_dust = "space:aerosol.dust";
inline constexpr std::string_view aerosol_haze = "space:aerosol.haze";

struct GasShare {
    std::string_view gas;
    double fraction = 0.0;  ///< mole fraction
};

/// One body's atmosphere, physical.
struct AtmosphereComposition {
    std::uint32_t index = 0;  ///< the body's index
    std::string_view kind = atmosphere_none;
    std::array<GasShare, 4> gases{};
    std::uint32_t gas_count = 0;
    double surface_pressure_pa = 0.0;
    double surface_temperature_k = 0.0;
    std::string_view aerosol = aerosol_clear;
    double aerosol_optical_depth = 0.0;  ///< vertical, at 550 nm
    double aerosol_scale_height_m = 0.0;
    double ozone_column_du = 0.0;  ///< Dobson units of an ozone-like absorber

    [[nodiscard]] bool present() const { return kind != atmosphere_none; }
};

using AtmosphereSet = std::vector<AtmosphereComposition>;

/// Every body's atmosphere, in body order. `ctx` is the stage's (its seed and streams).
[[nodiscard]] AtmosphereSet derive_all_atmospheres(const veach::procgen::StageContext& ctx, const SystemRecord& system,
                                                   const std::vector<BodyPhysics>& physics);

std::uint64_t stage_hash(const AtmosphereSet& r);

}  // namespace space::gen
