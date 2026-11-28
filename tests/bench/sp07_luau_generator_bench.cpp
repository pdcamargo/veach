// Spike SP-07 (VCH-083), throwaway: the climate hook in the Luau procgen sandbox, for the tables in
// docs/research/spikes/sp-07-luau-generator.md.
//   build/release/tests/veach-bench-sp07
// Prints the hook's input and output hashes (compare Debug and Release), cells/s (best of 7) for the sandbox as
// shipped and with each knob changed (bypass open = platform libm, no interrupt, optimization level 1), the same
// hook ported to C++ on dmath (and whether its hash is bit-equal to Luau's), and the per-call overhead of an empty
// hook with and without the climate hook's I/O.

#include <support/sp07_climate.hpp>
#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/procgen/sandbox.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>

namespace {

using Clock = std::chrono::steady_clock;
namespace dm = veach::dmath;

double best_seconds(int runs, const std::function<void()>& fn) {
    double best = 1e30;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        fn();
        best = std::min(best, std::chrono::duration<double>(Clock::now() - t0).count());
    }
    return best;
}

// The hook in C++, operation for operation (same order, so the same rounding).
double daily_insolation(double lat, double dec) {
    constexpr double pi = 3.14159265358979323846;
    const double c = -dm::tan(lat) * dm::tan(dec);
    double h0 = 0;
    if (c >= 1) {
        return 0;
    } else if (c <= -1) {
        h0 = pi;
    } else {
        h0 = dm::acos(c);
    }
    return (h0 * dm::sin(lat) * dm::sin(dec) + dm::cos(lat) * dm::cos(dec) * dm::sin(h0)) / pi;
}
double annual_insolation(double lat, double tilt) {
    constexpr double pi = 3.14159265358979323846;
    double sum = 0;
    for (int k = 0; k < 8; ++k) {
        sum += daily_insolation(lat, dm::asin(dm::sin(tilt) * dm::sin(2 * pi * k / 8)));
    }
    return sum / 8;
}
double band(double x, double center, double width) {
    const double d = (x - center) / width;
    return dm::exp(-d * d);
}
double lmax(double a, double b) {
    return b > a ? b : a;
}
double lmin(double a, double b) {
    return b < a ? b : a;
}
std::uint8_t biome(double t, double p, double alt) {
    if (alt < 0) {
        return t <= 271.35 ? 1 : 0;
    }
    if (alt > 4500) {
        return 12;
    }
    if (t < 263) {
        return 2;
    }
    if (t < 271) {
        return 3;
    }
    if (t < 278) {
        return 4;
    }
    if (t < 293) {
        return p < 500 ? 5 : p < 1500 ? 6 : 7;
    }
    return p < 250 ? 8 : p < 1200 ? 9 : p < 2200 ? 10 : 11;
}
void climate_cpp(const sp07::Body& body, const sp07::Inputs& in, sp07::Outputs& out) {
    const std::uint64_t seed = veach::hash_id("sp07:climate");
    for (std::uint32_t i = 0; i < sp07::cells; ++i) {
        const double lat = in.latitude[i], alt = in.altitude[i];
        const double alat = std::abs(lat);
        const double q = 4 * annual_insolation(lat, body.axial_tilt_rad);
        const double t = body.equilibrium_temperature_k * (0.55 * dm::pow(q, 0.25) + 0.45) + sp07::greenhouse_k -
                         0.0065 * lmax(alt, 0);
        const double circulation =
            2400 * band(lat, 0, 0.14) + 900 * band(alat, 0.95, 0.2) + 450 - 380 * band(alat, 0.52, 0.12);
        double humidity = dm::exp(0.07 * (t - 288));
        humidity = humidity < 0.05 ? 0.05 : humidity;
        humidity = humidity > 3 ? 3 : humidity;
        const double relief = alt > 0 ? 1 + 0.25 * lmin(alt, 3000) / 1000 - 0.3 * lmax(alt - 3000, 0) / 1000 : 1;
        const double jitter = 0.85 + 0.3 * veach::unit_double(veach::hash(seed, std::uint64_t{1}, std::uint64_t{i}));
        const double p = lmax(circulation * humidity * lmax(relief, 0.1) * jitter, 0);
        out.temperature[i] = t;
        out.precipitation[i] = p;
        out.biome[i] = biome(t, p, alt);
    }
}

}  // namespace

int main() {
    const sp07::Body body = sp07::pick_body();
    const sp07::Inputs in = sp07::make_inputs();
    const std::string source = sp07::read_hook();
    std::printf("body %s: T_eq %.3f K, tilt %.4f rad; inputs 0x%016llx\n", body.address.c_str(),
                body.equilibrium_temperature_k, body.axial_tilt_rad,
                static_cast<unsigned long long>(sp07::hash_inputs(body, in)));

    struct Variant {
        const char* name;
        veach::procgen::SandboxOptions options;
    };
    veach::procgen::SandboxOptions shipped;
    veach::procgen::SandboxOptions libm = shipped;
    libm.close_fastcall_bypass = false;
    veach::procgen::SandboxOptions no_interrupt = shipped;
    no_interrupt.safepoint_budget = 0;
    veach::procgen::SandboxOptions o1 = shipped;
    o1.optimization_level = 1;
    const Variant variants[]{{"sandbox (dmath, interrupt, O2)", shipped},
                             {"bypass open (libm fastcalls)", libm},
                             {"no interrupt callback", no_interrupt},
                             {"optimization level 1", o1}};
    std::uint64_t luau_hash = 0;
    for (const Variant& v : variants) {
        veach::procgen::Sandbox sandbox(v.options);
        auto hook = sandbox.load(source, "tests/scripts/sp07_climate.luau");
        if (!hook) {
            std::printf("%s\n", hook.error().to_string().c_str());
            return 1;
        }
        sp07::Outputs out;
        bool ok = true;
        const double s = best_seconds(7, [&] { ok = ok && sp07::run(sandbox, *hook, body, in, out).has_value(); });
        if (!ok) {
            std::printf("%s: hook failed\n", v.name);
            return 1;
        }
        const std::uint64_t h = sp07::hash_outputs(out);
        if (&v == &variants[0]) {
            luau_hash = h;
            std::uint32_t histogram[13]{};
            double t_min = 1e9, t_max = 0, p_max = 0;
            for (std::uint32_t c = 0; c < sp07::cells; ++c) {
                ++histogram[out.biome[c]];
                t_min = std::min(t_min, out.temperature[c]);
                t_max = std::max(t_max, out.temperature[c]);
                p_max = std::max(p_max, out.precipitation[c]);
            }
            std::printf("temperature %.1f..%.1f K, precipitation up to %.0f mm/yr; biome histogram:", t_min, t_max,
                        p_max);
            for (std::uint32_t b = 0; b < 13; ++b) {
                std::printf(" %u:%u", b, histogram[b]);
            }
            std::printf("\n");
        }
        std::printf(
            "%-32s %8.3f ms/call  %6.2f M cells/s  %7.0f ns/cell  outputs 0x%016llx  %.1f safepoints/cell  "
            "%.1f dmath calls/cell\n",
            v.name, s * 1e3, sp07::cells / s / 1e6, s / sp07::cells * 1e9, static_cast<unsigned long long>(h),
            static_cast<double>(sandbox.last_stats().safepoints) / sp07::cells,
            static_cast<double>(sandbox.last_stats().dmath_calls) / sp07::cells);
    }

    sp07::Outputs cpp_out;
    const double cpp_s = best_seconds(7, [&] { climate_cpp(body, in, cpp_out); });
    const std::uint64_t cpp_hash = sp07::hash_outputs(cpp_out);
    std::printf("%-32s %8.3f ms/call  %6.2f M cells/s  %7.0f ns/cell  outputs 0x%016llx  (%s Luau)\n",
                "C++ port on dmath", cpp_s * 1e3, sp07::cells / cpp_s / 1e6, cpp_s / sp07::cells * 1e9,
                static_cast<unsigned long long>(cpp_hash), cpp_hash == luau_hash ? "bit-equal to" : "DIFFERS from");

    // Per-call overhead: an empty hook, bare and with the climate hook's params and buffers (copy in and out).
    veach::procgen::Sandbox sandbox;
    auto empty = sandbox.load("return function(ctx, inputs, outputs) end", "empty.luau");
    if (!empty) {
        return 1;
    }
    constexpr int calls = 20000;
    const double bare = best_seconds(5, [&] {
        for (int i = 0; i < calls; ++i) {
            (void)sandbox.run(*empty, {});
        }
    });
    sp07::Outputs out;
    const double with_io = best_seconds(5, [&] {
        for (int i = 0; i < 200; ++i) {
            (void)sp07::run(sandbox, *empty, body, in, out);
        }
    });
    std::printf(
        "per-call overhead: empty hook %.2f us, with the climate I/O (%u cells, %zu KiB in, %zu KiB out) "
        "%.2f us\n",
        bare / calls * 1e6, sp07::cells, (2 * sizeof(double) * sp07::cells) >> 10,
        ((2 * sizeof(double) + 1) * sp07::cells) >> 10, with_io / 200 * 1e6);
    return 0;
}
