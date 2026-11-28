#pragma once

// Spike SP-07 (VCH-083), throwaway: inputs for the climate hook (tests/scripts/sp07_climate.luau), shared by
// tests/procgen_sandbox_tests.cpp and tests/bench/sp07_luau_generator_bench.cpp.
// docs/research/spikes/sp-07-luau-generator.md.
//
// A 64 x 64 x 6 cube map of latitude and altitude for one SP-05 body, all canonical code (veach::dmath, veach::hash,
// exact IEEE operations), so the inputs themselves are equal on every platform.

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/procgen/sandbox.hpp>
#include <veach/space/gen/system.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace sp07 {

inline constexpr std::uint32_t face_size = 64;
inline constexpr std::uint32_t cells = face_size * face_size * 6;
inline constexpr std::uint64_t terrain_seed = 0x5350303743ULL;  // "SP07C"
inline constexpr double greenhouse_k = 33.0;

struct Body {
    std::string address;
    double equilibrium_temperature_k = 0;
    double axial_tilt_rad = 0;
};

/// The first potentially habitable SP-05 body in the galaxy's central cell (universe seed 42, bias 0.5) that is
/// Earth-like enough for an interesting climate: not tidally locked, tilted over 0.1 rad, T_eq 230-290 K.
inline Body pick_body() {
    namespace sg = space::gen;
    static const sg::UniverseRecord universe{42, 0.5};
    for (std::uint64_t s = 0; s < 4096; ++s) {
        sg::SystemCoords c;
        c.x = 1u << (sg::cell_bits_xz(0) - 1u);
        c.z = c.x;
        c.y = 1u << (sg::cell_bits_y(0) - 1u);
        c.system = s;
        const sg::StageContext ctx{&universe, *sg::SystemAddress::encode(c)};
        const sg::SystemRecord r = sg::generate_system(ctx);
        for (const sg::BodyPhysics& p : sg::derive_all_body_physics(ctx, r)) {
            if (!p.potentially_habitable) {
                continue;
            }
            for (const sg::BodyRecord& b : r.bodies) {
                if (b.index == p.index && !b.tidally_locked && b.obliquity_rad > 0.1 &&
                    p.equilibrium_temperature_k > 230 && p.equilibrium_temperature_k < 290) {
                    return {sg::to_string(ctx.address.body(b.index)), p.equilibrium_temperature_k, b.obliquity_rad};
                }
            }
        }
    }
    return {};
}

inline double smooth(double t) {
    return t * t * (3.0 - 2.0 * t);
}

/// Value noise in [0, 1): hashed lattice values, smoothstep-weighted trilinear blend.
inline double value_noise(std::uint64_t seed, double x, double y, double z) {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy),
               iz = static_cast<std::int64_t>(fz);
    const double tx = smooth(x - fx), ty = smooth(y - fy), tz = smooth(z - fz);
    auto v = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        return veach::unit_double(veach::hash(seed, ix + dx, iy + dy, iz + dz));
    };
    auto lerp = [](double a, double b, double t) {
        return a + t * (b - a);
    };
    const double x00 = lerp(v(0, 0, 0), v(1, 0, 0), tx), x10 = lerp(v(0, 1, 0), v(1, 1, 0), tx);
    const double x01 = lerp(v(0, 0, 1), v(1, 0, 1), tx), x11 = lerp(v(0, 1, 1), v(1, 1, 1), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

struct Inputs {
    std::vector<double> latitude;  ///< rad, +Y is north
    std::vector<double> altitude;  ///< m above the datum; 58% of cells are below 0 (ocean)
};

/// Cell (face, i, j) is index (face * 64 + j) * 64 + i; faces +X -X +Y -Y +Z -Z.
inline Inputs make_inputs() {
    Inputs in;
    in.latitude.resize(cells);
    in.altitude.resize(cells);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t j = 0; j < face_size; ++j) {
            for (std::uint32_t i = 0; i < face_size; ++i) {
                const double u = 2.0 * (i + 0.5) / face_size - 1.0;
                const double v = 2.0 * (j + 0.5) / face_size - 1.0;
                double d[3]{};
                switch (face) {
                    case 0: d[0] = 1, d[1] = v, d[2] = -u; break;
                    case 1: d[0] = -1, d[1] = v, d[2] = u; break;
                    case 2: d[0] = u, d[1] = 1, d[2] = -v; break;
                    case 3: d[0] = u, d[1] = -1, d[2] = v; break;
                    case 4: d[0] = u, d[1] = v, d[2] = 1; break;
                    default: d[0] = -u, d[1] = v, d[2] = -1; break;
                }
                const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const double x = d[0] / len, y = d[1] / len, z = d[2] / len;
                const std::uint32_t cell = (face * face_size + j) * face_size + i;
                in.latitude[cell] = veach::dmath::asin(y);
                double n = 0, amp = 0.5, freq = 2.0, norm = 0;
                for (std::uint64_t octave = 0; octave < 4; ++octave) {
                    n += amp * value_noise(veach::hash(terrain_seed, octave), x * freq, y * freq, z * freq);
                    norm += amp;
                    amp *= 0.5;
                    freq *= 2.0;
                }
                in.altitude[cell] = (n / norm - 0.55) * 16000.0;
            }
        }
    }
    return in;
}

struct Outputs {
    std::vector<double> temperature = std::vector<double>(cells);    ///< K
    std::vector<double> precipitation = std::vector<double>(cells);  ///< mm/yr
    std::vector<std::uint8_t> biome = std::vector<std::uint8_t>(cells);
};

inline std::string read_hook() {
    std::ifstream f(VEACH_SOURCE_DIR "/tests/scripts/sp07_climate.luau", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline veach::Result<> run(veach::procgen::Sandbox& sandbox, veach::procgen::Sandbox::HookId hook, const Body& body,
                           const Inputs& in, Outputs& out) {
    const veach::procgen::HookParam params[]{
        {"cells", static_cast<double>(cells)},
        {"equilibrium_temperature_k", body.equilibrium_temperature_k},
        {"axial_tilt_rad", body.axial_tilt_rad},
        {"greenhouse_k", greenhouse_k},
    };
    const veach::procgen::HookInput inputs[]{
        {"latitude", std::as_bytes(std::span(in.latitude))},
        {"altitude", std::as_bytes(std::span(in.altitude))},
    };
    const veach::procgen::HookOutput outputs[]{
        {"temperature", std::as_writable_bytes(std::span(out.temperature))},
        {"precipitation", std::as_writable_bytes(std::span(out.precipitation))},
        {"biome", std::as_writable_bytes(std::span(out.biome))},
    };
    return sandbox.run(hook, {veach::hash_id("sp07:climate"), params, inputs, outputs});
}

inline std::uint64_t bits(double x) {
    std::uint64_t b = 0;
    std::memcpy(&b, &x, sizeof b);
    return b;
}

/// Hash of every output, cell by cell (doubles by their bits; the hook never produces NaN).
inline std::uint64_t hash_outputs(const Outputs& out) {
    std::uint64_t h = veach::hash_id("sp07:climate.outputs");
    for (std::uint32_t c = 0; c < cells; ++c) {
        h = veach::hash(h, bits(out.temperature[c]), bits(out.precipitation[c]), out.biome[c]);
    }
    return h;
}

/// Hash of the inputs, to tell an input change from a hook change.
inline std::uint64_t hash_inputs(const Body& body, const Inputs& in) {
    std::uint64_t h = veach::hash(veach::hash_id("sp07:climate.inputs"), bits(body.equilibrium_temperature_k),
                                  bits(body.axial_tilt_rad));
    for (std::uint32_t c = 0; c < cells; ++c) {
        h = veach::hash(h, bits(in.latitude[c]), bits(in.altitude[c]));
    }
    return h;
}

}  // namespace sp07
