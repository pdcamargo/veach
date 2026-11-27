// veach::dmath benchmark and 10^7-input hashes (docs/research/spikes/sp-02-dmath.md, VCH-078).
//   build/release/tests/veach-bench-dmath [--inputs N] [--hash-only]
// For every function: ns per call of veach::dmath and of the platform's libm (std::) over N inputs in the function's
// typical range (best of 3 runs), then the hash of dmath's results over the first N inputs of the shared input set
// (tests/support/dmath_vectors.hpp: edge cases, typical, any finite value, raw bits). With the default N = 10^7 the
// hashes are checked against the committed ones below and the exit code is 1 on a mismatch, so the same binary proves
// bit-equality on the Mac and on Windows.

#include <veach/core/dmath.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "support/dmath_vectors.hpp"

namespace {

namespace dv = veach::test::dmath_vectors;
namespace dm = veach::dmath;

using D2 = double (*)(double, double);
using F2 = float (*)(float, float);

struct Function {
    const char* name;
    const dv::Domain* domain;
    D2 dmath;
    D2 libm;
    F2 dmath_f;
    F2 libm_f;
    std::uint64_t hash;    ///< dmath over 10^7 inputs, double.
    std::uint64_t hash_f;  ///< dmath over 10^7 inputs, float.
};

// clang-format off
#define VEACH_UNARY(n, h, hf)                                                                             \
    Function{#n, &dv::domains::n, [](double x, double) { return dm::n(x); },                             \
             [](double x, double) { return std::n(x); }, [](float x, float) { return dm::n(x); },        \
             [](float x, float) { return std::n(x); }, h, hf}
#define VEACH_BINARY(n, h, hf)                                                                            \
    Function{#n, &dv::domains::n, [](double x, double y) { return dm::n(x, y); },                        \
             [](double x, double y) { return std::n(x, y); }, [](float x, float y) { return dm::n(x, y); }, \
             [](float x, float y) { return std::n(x, y); }, h, hf}

// Committed 10^7-input hashes. Regenerate only on purpose: a change is a dmath version bump (dmath.hpp).
const Function functions[] = {
    VEACH_UNARY(sin, 0xbc8c78a3dd2abf00ULL, 0x4166eb69819224b6ULL),
    VEACH_UNARY(cos, 0x7ca706dfa90f8880ULL, 0xc526b87df16bc42dULL),
    VEACH_UNARY(tan, 0x2d94b75dd05e0ccdULL, 0xed437ab4a46d8730ULL),
    VEACH_UNARY(asin, 0x63445983dc1428b4ULL, 0x4af712fdbd7e7a6aULL),
    VEACH_UNARY(acos, 0x4cd600ac087f9df6ULL, 0xf45f0866435c67caULL),
    VEACH_UNARY(atan, 0x9af445bd9e883859ULL, 0x7b0bd074d78e53bbULL),
    VEACH_BINARY(atan2, 0xd1e73afc6dd2fb7eULL, 0x2a785cf27e8f5f14ULL),
    VEACH_UNARY(exp, 0xcd9b14832ccdd7f2ULL, 0x341dbee74ff7399eULL),
    VEACH_UNARY(exp2, 0x2fc256ab204d6223ULL, 0x9467f1486b091784ULL),
    VEACH_UNARY(log, 0x1e25cd6d4501c672ULL, 0x50f84bed7cc10a71ULL),
    VEACH_UNARY(log2, 0xb86772a8088bb4fdULL, 0xab7a9c5a617f261eULL),
    VEACH_UNARY(log10, 0x214af709f6394293ULL, 0x077ef44f3c4c3fe4ULL),
    VEACH_BINARY(pow, 0x68a167a9976fd85bULL, 0xbb23deaade335892ULL),
    VEACH_UNARY(cbrt, 0x1e120f3dd371de88ULL, 0x5d59782218d765c8ULL),
    VEACH_BINARY(hypot, 0xe9c1ed97fea01569ULL, 0xd52a2b6a9c30f38bULL),
    VEACH_UNARY(sinh, 0x8ac4c647e6805538ULL, 0xa4e441c4fe9f0399ULL),
    VEACH_UNARY(cosh, 0x0e8179a41898b054ULL, 0x143845d5a2a02675ULL),
    VEACH_UNARY(tanh, 0x40b0c9aeac7e8a97ULL, 0x2e1aef44bd4f3938ULL),
};
// clang-format on

constexpr std::uint64_t committed_inputs = 10'000'000;

/// Best of three runs of `f` over the inputs, in ns per call.
template <typename T, typename Fn>
double time_ns(Fn f, const std::vector<T>& xs, const std::vector<T>& ys, std::vector<T>& out) {
    double best = 1e30;
    for (int run = 0; run < 3; ++run) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < xs.size(); ++i) {
            out[i] = f(xs[i], ys[i]);
        }
        const auto end = std::chrono::steady_clock::now();
        best = std::min(best,
                        std::chrono::duration<double, std::nano>(end - start).count() / static_cast<double>(xs.size()));
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t inputs = committed_inputs;
    bool timing = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--inputs") == 0 && i + 1 < argc) {
            inputs = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--hash-only") == 0) {
            timing = false;
        } else {
            std::fprintf(stderr, "usage: veach-bench-dmath [--inputs N] [--hash-only]\n");
            return 2;
        }
    }
    const bool check = inputs == committed_inputs;
    const auto n = static_cast<std::size_t>(inputs);
    std::vector<double> xs(timing ? n : 0), ys(timing ? n : 0), out(timing ? n : 0);
    std::vector<float> xf(timing ? n : 0), yf(timing ? n : 0), outf(timing ? n : 0);

    std::printf("%-6s %10s %10s %10s %10s  %-18s %-18s\n", "", "dmath ns", "libm ns", "dmathf ns", "libmf ns",
                "hash (double)", "hash (float)");
    int failures = 0;
    for (const Function& f : functions) {
        double t[4] = {0, 0, 0, 0};
        if (timing) {
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint64_t h = dv::mix(f.domain->key ^ dv::mix(i) ^ 0xbe7c4ULL);
                xs[i] = dv::uniform(h, f.domain->x_lo, f.domain->x_hi);
                ys[i] = dv::uniform(dv::mix(h), f.domain->y_lo, f.domain->y_hi);
                xf[i] = static_cast<float>(xs[i]);
                yf[i] = static_cast<float>(ys[i]);
            }
            t[0] = time_ns(f.dmath, xs, ys, out);
            t[1] = time_ns(f.libm, xs, ys, out);
            t[2] = time_ns(f.dmath_f, xf, yf, outf);
            t[3] = time_ns(f.libm_f, xf, yf, outf);
        }
        dv::Hash hash, hash_f;
        for (std::uint64_t i = 0; i < inputs; ++i) {
            const dv::Input in = dv::input(*f.domain, i);
            hash.add(f.dmath(in.x, in.y));
            const dv::InputF in_f = dv::input_f(*f.domain, i);
            hash_f.add(f.dmath_f(in_f.x, in_f.y));
        }
        const bool ok = !check || (hash.value == f.hash && hash_f.value == f.hash_f);
        failures += ok ? 0 : 1;
        std::printf("%-6s %10.2f %10.2f %10.2f %10.2f  %016llx   %016llx%s\n", f.name, t[0], t[1], t[2], t[3],
                    static_cast<unsigned long long>(hash.value), static_cast<unsigned long long>(hash_f.value),
                    ok ? "" : "  MISMATCH");
    }
    if (check) {
        if (failures == 0) {
            std::printf("dmath: all 10^7-input hashes match the committed ones\n");
        } else {
            std::printf("dmath: %d functions differ from the committed hashes\n", failures);
        }
    }
    return failures == 0 ? 0 : 1;
}
