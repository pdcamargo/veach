// Spike SP-09 (VCH-085), THROWAWAY: planet and asteroid density fields with box bounds (sp09_terrain.hpp).
// + - x / sqrt floor only in the sample path (ADR 0043).

#include "sp09_terrain.hpp"

#include <veach/core/dmath.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace lab::sp09 {

namespace {

/// Value noise with a smoothstep fade changes by at most 1.5 x 2 per lattice unit along an axis: 3 sqrt(3) in any
/// direction.
constexpr double lipschitz = 5.196152422706632;
constexpr double earth_radius = 6'371'000.0;

constexpr std::uint32_t lowbias32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline double corner(std::uint32_t seed, std::int32_t ix, std::int32_t iy, std::int32_t iz) {
    const std::uint32_t h =
        lowbias32(seed ^ (static_cast<std::uint32_t>(ix) * 0x8da6b343U) ^
                  (static_cast<std::uint32_t>(iy) * 0xd8163841U) ^ (static_cast<std::uint32_t>(iz) * 0xcb1ab31fU));
    return static_cast<double>(h >> 8) * (2.0 / 16777216.0) - 1.0;
}

inline double fade(double t) {
    return t * t * (3.0 - 2.0 * t);
}

inline double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

/// Value noise in [-1, 1), wavelength 1.
inline double value_noise(std::uint32_t seed, double x, double y, double z) {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const auto ix = static_cast<std::int32_t>(fx), iy = static_cast<std::int32_t>(fy),
               iz = static_cast<std::int32_t>(fz);
    const double tx = fade(x - fx), ty = fade(y - fy), tz = fade(z - fz);
    const double x00 = lerp(corner(seed, ix, iy, iz), corner(seed, ix + 1, iy, iz), tx);
    const double x10 = lerp(corner(seed, ix, iy + 1, iz), corner(seed, ix + 1, iy + 1, iz), tx);
    const double x01 = lerp(corner(seed, ix, iy, iz + 1), corner(seed, ix + 1, iy, iz + 1), tx);
    const double x11 = lerp(corner(seed, ix, iy + 1, iz + 1), corner(seed, ix + 1, iy + 1, iz + 1), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

struct Interval {
    double lo = 0.0;
    double hi = 0.0;
};

Interval operator+(Interval a, Interval b) {
    return {a.lo + b.lo, a.hi + b.hi};
}
Interval operator*(double k, Interval a) {
    return k >= 0.0 ? Interval{k * a.lo, k * a.hi} : Interval{k * a.hi, k * a.lo};
}
Interval operator*(Interval a, Interval b) {
    const std::array<double, 4> p{a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    return {*std::ranges::min_element(p), *std::ranges::max_element(p)};
}
Interval clamp01(Interval a) {
    return {std::clamp(a.lo, 0.0, 1.0), std::clamp(a.hi, 0.0, 1.0)};
}

/// Fractal sum of value noise: `octaves` octaves from `frequency` (per input unit), doubling, amplitude x `gain`.
struct Fbm {
    static constexpr int max_octaves = 12;
    std::array<std::uint32_t, max_octaves> seeds{};
    int octaves = 1;
    double frequency = 1.0;
    double amplitude = 1.0;
    double gain = 0.5;
    bool ridged = false;  ///< sum of (1 - |v|)^2, normalized to [0, 1]

    Fbm(std::uint32_t seed, int count, double freq, double amp, bool ridge = false)
        : octaves(count), frequency(freq), amplitude(amp), ridged(ridge) {
        for (int i = 0; i < octaves; ++i) {
            seeds[static_cast<std::size_t>(i)] = lowbias32(seed + 0x9e3779b9U * static_cast<std::uint32_t>(i + 1));
        }
    }

    /// Sum of the octave amplitudes.
    double total() const {
        double a = amplitude, t = 0.0;
        for (int i = 0; i < octaves; ++i) {
            t += a;
            a *= gain;
        }
        return t;
    }

    double operator()(double x, double y, double z) const {
        double sum = 0.0, a = amplitude, f = frequency;
        for (int i = 0; i < octaves; ++i) {
            const double v = value_noise(seeds[static_cast<std::size_t>(i)], x * f, y * f, z * f);
            sum += ridged ? a * (1.0 - std::abs(v)) * (1.0 - std::abs(v)) : a * v;
            a *= gain;
            f *= 2.0;
        }
        return ridged ? sum / total() : sum;
    }

    /// Bounds over the ball of radius `r` (input units) around (x, y, z).
    Interval bounds(double x, double y, double z, double r) const {
        Interval out;
        double a = amplitude, f = frequency;
        for (int i = 0; i < octaves; ++i) {
            const double v = value_noise(seeds[static_cast<std::size_t>(i)], x * f, y * f, z * f);
            const double spread = lipschitz * f * r;
            const double lo = std::max(-1.0, v - spread), hi = std::min(1.0, v + spread);
            if (ridged) {
                const double abs_lo = (lo <= 0.0 && hi >= 0.0) ? 0.0 : std::min(std::abs(lo), std::abs(hi));
                const double abs_hi = std::max(std::abs(lo), std::abs(hi));
                out.lo += a * (1.0 - abs_hi) * (1.0 - abs_hi);
                out.hi += a * (1.0 - abs_lo) * (1.0 - abs_lo);
            } else {
                out.lo += a * lo;
                out.hi += a * hi;
            }
            a *= gain;
            f *= 2.0;
        }
        if (ridged) {
            const double t = total();
            out.lo /= t;
            out.hi /= t;
        }
        return out;
    }

    Interval range() const { return ridged ? Interval{0.0, 1.0} : Interval{-total(), total()}; }
};

// The planet's generator. Planetary band: unit-sphere inputs (frequency in units of 1/R).
constexpr double continent_height = 3000.0;  // meters at s = 1
constexpr double mountain_height = 9000.0;   // meters at s = 1
constexpr double hill_amplitude = 20.0;      // meters, never scaled
constexpr double hill_mountain_amplitude = 40.0;

inline Interval mask_of(Interval c) {
    return clamp01(4.0 * Interval{c.lo + 0.05, c.hi + 0.05});
}

/// Distances from the origin to the nearest and farthest points of a box.
void radial_range(const double lo[3], const double hi[3], double& near, double& far) {
    double n2 = 0.0, f2 = 0.0;
    for (int a = 0; a < 3; ++a) {
        const double n = lo[a] > 0.0 ? lo[a] : (hi[a] < 0.0 ? -hi[a] : 0.0);
        const double f = std::max(std::abs(lo[a]), std::abs(hi[a]));
        n2 += n * n;
        f2 += f * f;
    }
    near = std::sqrt(n2);
    far = std::sqrt(f2);
}

BoxClass classify_distance(double d_lo, double d_hi) {
    constexpr double margin = 1.0;  // meters: well over half a code step
    if (d_lo > margin) {
        return BoxClass::empty;
    }
    if (d_hi < -margin) {
        return BoxClass::solid;
    }
    return BoxClass::surface;
}

}  // namespace

struct PlanetNoise {
    Fbm continents;
    Fbm mountains;
    Fbm detail;
    explicit PlanetNoise(std::uint32_t seed)
        : continents(seed ^ 0x1111U, 5, 1.6, 1.0),
          mountains(seed ^ 0x2222U, 8, 12.0, 1.0, true),
          detail(seed ^ 0x3333U, 8, 1.0 / 600.0, 1.0) {}
};

struct AsteroidNoise {
    Fbm lumps;
    Fbm detail;
    explicit AsteroidNoise(std::uint32_t seed)
        : lumps(seed ^ 0x4444U, 4, 1.0 / 420.0, 120.0), detail(seed ^ 0x5555U, 6, 1.0 / 60.0, 6.0) {}
};

PlanetField::PlanetField(PlanetSettings settings)
    : settings_(settings),
      noise_(std::make_shared<const PlanetNoise>(settings.seed)),
      radius_(earth_radius * settings.scale),
      relief_(settings.relief_exponent == 0.0 ? 1.0 : veach::dmath::pow(settings.scale, settings.relief_exponent)) {}

double PlanetField::max_radius() const {
    const PlanetNoise& n = *noise_;
    return radius_ + relief_ * (continent_height * n.continents.total() + mountain_height) +
           (hill_amplitude + hill_mountain_amplitude) * n.detail.total();
}

PlanetField::Height PlanetField::height(double nx, double ny, double nz) const {
    const PlanetNoise& n = *noise_;
    const double c = n.continents(nx, ny, nz);
    const double mask = std::clamp((c + 0.05) * 4.0, 0.0, 1.0);
    const double m = mask > 0.0 ? n.mountains(nx, ny, nz) : 0.0;
    const double band = relief_ * (continent_height * c + mountain_height * mask * m);
    const double detail = n.detail(nx * radius_, ny * radius_, nz * radius_);
    return {band + (hill_amplitude + hill_mountain_amplitude * mask) * detail, band, mask};
}

double PlanetField::planetary_height(double nx, double ny, double nz) const {
    return height(nx, ny, nz).band;
}

double PlanetField::surface_height(double px, double py, double pz) const {
    const double r = std::sqrt(px * px + py * py + pz * pz);
    return height(px / r, py / r, pz / r).h;
}

BoxClass PlanetField::classify(const double lo[3], const double hi[3]) const {
    const PlanetNoise& n = *noise_;
    double near = 0.0, far = 0.0;
    radial_range(lo, hi, near, far);
    const double cx = 0.5 * (lo[0] + hi[0]), cy = 0.5 * (lo[1] + hi[1]), cz = 0.5 * (lo[2] + hi[2]);
    const double hx = 0.5 * (hi[0] - lo[0]), hy = 0.5 * (hi[1] - lo[1]), hz = 0.5 * (hi[2] - lo[2]);
    const double rho = std::sqrt(hx * hx + hy * hy + hz * hz);
    const double rc = std::sqrt(cx * cx + cy * cy + cz * cz);

    Interval c, m, detail;
    if (rho >= 0.9 * rc) {
        c = n.continents.range();
        m = n.mountains.range();
        detail = n.detail.range();
    } else {
        // Directions of the box lie within angle asin(rho / rc) of the center's: a chord of at most that angle.
        const double angle = std::asin(rho / rc);
        const double ux = cx / rc, uy = cy / rc, uz = cz / rc;
        c = n.continents.bounds(ux, uy, uz, angle);
        m = n.mountains.bounds(ux, uy, uz, angle);
        detail = n.detail.bounds(ux * radius_, uy * radius_, uz * radius_, angle * radius_);
    }
    const Interval mask = mask_of(c);
    const Interval band = relief_ * ((continent_height * c) + mountain_height * (mask * m));
    const Interval amplitude{hill_amplitude + hill_mountain_amplitude * mask.lo,
                             hill_amplitude + hill_mountain_amplitude * mask.hi};
    const Interval h = band + amplitude * detail;
    return classify_distance(near - radius_ - h.hi, far - radius_ - h.lo);
}

veach::voxel::Sample PlanetField::sample(std::int64_t x, std::int64_t y, std::int64_t z) const {
    const double px = static_cast<double>(x) * veach::voxel::lod0_cell_meters;
    const double py = static_cast<double>(y) * veach::voxel::lod0_cell_meters;
    const double pz = static_cast<double>(z) * veach::voxel::lod0_cell_meters;
    const double r = std::sqrt(px * px + py * py + pz * pz);
    if (r < 1.0) {
        return {veach::voxel::encode_distance(-radius_, settings_.encoding), Material::rock};
    }
    const Height h = height(px / r, py / r, pz / r);
    const double d = r - radius_ - h.h;
    std::uint8_t material = Material::lowland;
    if (h.band > relief_ * 8500.0) {
        material = Material::snow;
    } else if (h.mask > 0.6 && h.band > relief_ * 3500.0) {
        material = Material::rock;
    } else if (h.band > relief_ * 500.0) {
        material = Material::highland;
    }
    return {veach::voxel::encode_distance(d, settings_.encoding), material};
}

// The asteroid: two 3D fractal sums in meters.
AsteroidField::AsteroidField(AsteroidSettings settings)
    : settings_(settings), noise_(std::make_shared<const AsteroidNoise>(settings.seed)) {}

double AsteroidField::lumps(double px, double py, double pz) const {
    const AsteroidNoise& n = *noise_;
    return n.lumps(px, py, pz) + n.detail(px, py, pz);
}

double AsteroidField::max_radius() const {
    const AsteroidNoise& n = *noise_;
    return settings_.radius + n.lumps.total() + n.detail.total();
}

double AsteroidField::surface_height(double px, double py, double pz) const {
    const double r = std::sqrt(px * px + py * py + pz * pz);
    const double ux = px / r, uy = py / r, uz = pz / r;
    double h = 0.0;
    for (int i = 0; i < 6; ++i) {  // fixed point of |p| = R + lumps(p) along the ray
        const double s = settings_.radius + h;
        h = lumps(ux * s, uy * s, uz * s);
    }
    return h;
}

BoxClass AsteroidField::classify(const double lo[3], const double hi[3]) const {
    const AsteroidNoise& n = *noise_;
    double near = 0.0, far = 0.0;
    radial_range(lo, hi, near, far);
    const double cx = 0.5 * (lo[0] + hi[0]), cy = 0.5 * (lo[1] + hi[1]), cz = 0.5 * (lo[2] + hi[2]);
    const double hx = 0.5 * (hi[0] - lo[0]), hy = 0.5 * (hi[1] - lo[1]), hz = 0.5 * (hi[2] - lo[2]);
    const double rho = std::sqrt(hx * hx + hy * hy + hz * hz);
    const Interval h = n.lumps.bounds(cx, cy, cz, rho) + n.detail.bounds(cx, cy, cz, rho);
    return classify_distance(near - settings_.radius - h.hi, far - settings_.radius - h.lo);
}

veach::voxel::Sample AsteroidField::sample(std::int64_t x, std::int64_t y, std::int64_t z) const {
    const double px = static_cast<double>(x) * veach::voxel::lod0_cell_meters;
    const double py = static_cast<double>(y) * veach::voxel::lod0_cell_meters;
    const double pz = static_cast<double>(z) * veach::voxel::lod0_cell_meters;
    const double r = std::sqrt(px * px + py * py + pz * pz);
    const double d = r - settings_.radius - lumps(px, py, pz);
    return {veach::voxel::encode_distance(d, settings_.encoding), Material::regolith};
}

}  // namespace lab::sp09
