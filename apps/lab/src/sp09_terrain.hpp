#pragma once

// Spike SP-09 (VCH-085), THROWAWAY: the density fields of the descent (docs/research/spikes/sp-09-descent.md).
//
// Two bodies, both veach::voxel::DensityFields in body-local meters (ADR 0053), pure functions of the lattice point
// (ADR 0074 decision 1: no band limit, whatever level asks):
// - PlanetField: a sphere of the realized radius R = 6,371 km x s, a planetary-band heightfield on the direction
//   (continents and ridged mountains, frequencies in units of R, heights in meters x s^relief_exponent, ADR 0049) and
//   a detail band in meters that never scales (hills down to 4.7 m, evaluated on the reference sphere so the result
//   stays a heightfield). d = |p| - R - h(p / |p|).
// - AsteroidField: fully 3D (ADR 0053): a lumpy 500 m sphere with 3D noise in meters.
//
// Both give conservative bounds over a chunk box (classify()): every noise octave is Lipschitz (value noise with a
// smoothstep fade changes by at most 3*sqrt(3) per lattice unit), so one evaluation at the box center plus the box
// radius bounds the field over the box. This is the min/max skipping ADR 0053 asks for, without a cached pyramid.
//
// Samples: int16 codes, either ADR 0053's linear encoding (1/1024 m, clamped to +-32 m) or the companded one this spike
// adds to engine/voxel (linear to +-16 m, logarithmic beyond), so coarse cells (up to 262 km) keep their distances.

#include <veach/voxel/voxel.hpp>

#include <cstdint>
#include <memory>

namespace lab::sp09 {

struct PlanetNoise;
struct AsteroidNoise;

using Encoding = veach::voxel::DistanceEncoding;

/// Box classes for streaming: nothing to sample (all air or all rock) or maybe a surface.
enum class BoxClass : std::uint8_t { empty, solid, surface };

struct PlanetSettings {
    double scale = 0.1;            ///< s_p
    double relief_exponent = 0.5;  ///< planetary relief x s^exponent (ADR 0049, 0066)
    std::uint32_t seed = 0x5109;
    Encoding encoding = Encoding::companded;
};

/// Materials in Sample::material.
enum Material : std::uint8_t { lowland = 0, highland = 1, rock = 2, snow = 3, regolith = 4 };

class TerrainField : public veach::voxel::DensityField {
public:
    /// The body's reference radius, meters (the planet's sea-level sphere, the asteroid's mean radius).
    virtual double radius() const = 0;
    /// The largest |p| the surface reaches, meters (for the octree roots).
    virtual double max_radius() const = 0;
    /// Height of the surface above radius() along the direction of `p` (meters; p need not be unit).
    virtual double surface_height(double px, double py, double pz) const = 0;
    /// Conservative class of the closed box [lo, hi] (meters).
    virtual BoxClass classify(const double lo[3], const double hi[3]) const = 0;
};

class PlanetField final : public TerrainField {
public:
    explicit PlanetField(PlanetSettings settings);

    double radius() const override { return radius_; }
    double max_radius() const override;
    double surface_height(double px, double py, double pz) const override;
    BoxClass classify(const double lo[3], const double hi[3]) const override;
    veach::voxel::Sample sample(std::int64_t x, std::int64_t y, std::int64_t z) const override;

    /// The planetary band alone at a unit direction (meters, already x s^exponent).
    double planetary_height(double nx, double ny, double nz) const;
    const PlanetSettings& settings() const { return settings_; }

private:
    struct Height {
        double h;     ///< total, meters
        double band;  ///< the planetary band, meters
        double mask;  ///< mountains, 0..1
    };
    Height height(double nx, double ny, double nz) const;

    PlanetSettings settings_;
    std::shared_ptr<const PlanetNoise> noise_;
    double radius_ = 0.0;
    double relief_ = 1.0;  ///< s^exponent
};

struct AsteroidSettings {
    double radius = 500.0;  ///< meters (a 1 km asteroid)
    std::uint32_t seed = 0xA57E;
    Encoding encoding = Encoding::companded;
};

class AsteroidField final : public TerrainField {
public:
    explicit AsteroidField(AsteroidSettings settings);

    double radius() const override { return settings_.radius; }
    double max_radius() const override;
    double surface_height(double px, double py, double pz) const override;
    BoxClass classify(const double lo[3], const double hi[3]) const override;
    veach::voxel::Sample sample(std::int64_t x, std::int64_t y, std::int64_t z) const override;

private:
    double lumps(double px, double py, double pz) const;
    AsteroidSettings settings_;
    std::shared_ptr<const AsteroidNoise> noise_;
};

}  // namespace lab::sp09
