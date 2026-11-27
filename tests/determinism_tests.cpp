// Canonical floating point (docs/adr/0043-determinism-tiers-and-strict-floating-point.md, VCH-077): the same
// source gives the same bits on macOS arm64 (Apple Clang) and Windows x64 (MSVC). The computation below is full of
// `a * b + c`, which a compiler that contracts to FMA rounds differently; the committed hash holds only when every
// CPU target is built with contraction off (cmake/VeachCompiler.cmake).
//
// veach::dmath (VCH-078, docs/research/spikes/sp-02-dmath.md): every function hashed over a fixed input set (edge
// cases, typical arguments, any finite value, raw bits) against committed hashes, and checked on hard-to-round cases
// against the correctly rounded results.

#include <doctest/doctest.h>
#include <veach/core/dmath.hpp>

#include <bit>
#include <cstdint>
#include <format>
#include <string_view>

#include "support/dmath_vectors.hpp"

namespace {

/// PCG-style integer hash: deterministic everywhere, no floating point.
std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/// A double in [-1, 1) from integer bits, exactly (53 bits of mantissa).
double unit(std::uint64_t bits) {
    return static_cast<double>(static_cast<std::int64_t>(bits >> 11) - (std::int64_t{1} << 52)) * 0x1p-52;
}

/// Horner polynomials, dot products and a damped recurrence over runtime inputs; hashes every intermediate result.
std::uint64_t canonical_hash(std::uint64_t seed) {
    constexpr double coefficients[] = {0.3, -1.7, 2.9, 0.11, -0.45, 1.3, -0.07, 0.9};
    std::uint64_t hash = 14695981039346656037ULL;
    double state = 0.5;
    for (std::uint64_t i = 0; i < 4096; ++i) {
        const double x = unit(mix(seed + i));
        const double y = unit(mix(seed ^ (i * 0x9e3779b97f4a7c15ULL)));
        double horner = 0.0;
        for (const double c : coefficients) {
            horner = horner * x + c;
        }
        const double dot = x * y + state * horner - y * state;
        state = state * 0.75 + dot * 0.25 / (1.0 + x * x);
        hash = (hash ^ std::bit_cast<std::uint64_t>(horner)) * 1099511628211ULL;
        hash = (hash ^ std::bit_cast<std::uint64_t>(dot)) * 1099511628211ULL;
        hash = (hash ^ std::bit_cast<std::uint64_t>(state)) * 1099511628211ULL;
    }
    return hash;
}

}  // namespace

TEST_CASE("[adr0043] canonical floating point gives the same bits on every platform") {
    // `volatile` keeps the compiler from folding the whole computation at compile time.
    volatile std::uint64_t seed = 0x5eed;
    const std::uint64_t hash = canonical_hash(seed);
    INFO(std::format("canonical hash: 0x{:016x}", hash));
    // Regenerate only on purpose: a different value means the build contracts or reassociates floating point.
    // Apple Clang without -ffp-contract=off gives 0x09763f25784f94bd at -O0 and -O2 (checked when this was committed).
    CHECK(hash == 0xd22b4dba96c3f2d5ULL);
}

namespace {

namespace dv = veach::test::dmath_vectors;
namespace dm = veach::dmath;

struct DmathCase {
    const char* name;
    const dv::Domain* domain;
    double (*d)(double, double);
    float (*f)(float, float);
    std::uint64_t hash;    ///< Over dmath_inputs inputs, double.
    std::uint64_t hash_f;  ///< Same, float.
};

constexpr std::uint64_t dmath_inputs = 100'000;

// clang-format off
#define VEACH_UNARY(n, h, hf) \
    DmathCase{#n, &dv::domains::n, [](double x, double) { return dm::n(x); }, [](float x, float) { return dm::n(x); }, h, hf}
#define VEACH_BINARY(n, h, hf) \
    DmathCase{#n, &dv::domains::n, [](double x, double y) { return dm::n(x, y); }, \
              [](float x, float y) { return dm::n(x, y); }, h, hf}
// clang-format on

// Regenerate only on purpose: a different value is a dmath version bump (veach/core/dmath.hpp). The 10^7-input
// hashes are in tests/bench/dmath_bench.cpp.
const DmathCase dmath_cases[] = {
    VEACH_UNARY(sin, 0xbb84e4600234a8f7ULL, 0x9e2d62720369c318ULL),
    VEACH_UNARY(cos, 0x268ceb6086e40d8aULL, 0xb1d4e6400616c543ULL),
    VEACH_UNARY(tan, 0x11b20f7aa42dcf13ULL, 0xc33ce235ecb65a32ULL),
    VEACH_UNARY(asin, 0x88026bb91c82f4a4ULL, 0xbd4a5442f866d647ULL),
    VEACH_UNARY(acos, 0xbedd14cce496bf38ULL, 0xa449238ec3a5ae2bULL),
    VEACH_UNARY(atan, 0x595ff7ee6461f5eaULL, 0xba4de38383df9d35ULL),
    VEACH_BINARY(atan2, 0x96df6d2c68fdebcaULL, 0x8feb5f1f6e95a0c2ULL),
    VEACH_UNARY(exp, 0xbcb3ae89af47a0d5ULL, 0x34bb52d2db6d0c4bULL),
    VEACH_UNARY(exp2, 0x1f78b48a8024f433ULL, 0xa5153d2c39c33f6bULL),
    VEACH_UNARY(log, 0x0de4b1fd1d118646ULL, 0x1918570a521479e4ULL),
    VEACH_UNARY(log2, 0xfc7a714c141b70d8ULL, 0x0feaedd0de77a108ULL),
    VEACH_UNARY(log10, 0x4b1843a34d85e84dULL, 0xdc89fc4e2b08ecdfULL),
    VEACH_BINARY(pow, 0x6d19454c45c1ae44ULL, 0xa09e6803863900fdULL),
    VEACH_UNARY(cbrt, 0xda2e815c2b7ea5c8ULL, 0x9a31b3cccc7ad4edULL),
    VEACH_BINARY(hypot, 0xf8931d24e0c31650ULL, 0xc126e4a854fa894fULL),
    VEACH_UNARY(sinh, 0x76d8ca01a2397216ULL, 0x5750397128d80523ULL),
    VEACH_UNARY(cosh, 0xf9b6c33a07ac6ed3ULL, 0x7f00c58d9b2e40c9ULL),
    VEACH_UNARY(tanh, 0x7713168546d2b0d2ULL, 0x386c71d7589f8dfaULL),
};

#undef VEACH_UNARY
#undef VEACH_BINARY

struct HardCase {
    const char* name;
    double x;
    double y;
    std::uint64_t expected;
    bool correctly_rounded;
};

struct HardCaseF {
    const char* name;  ///< With the C suffix: "powf".
    float x;
    float y;
    std::uint32_t expected;
};

#include "support/dmath_hard_cases.inl"

const DmathCase* dmath_case(std::string_view name) {
    for (const DmathCase& c : dmath_cases) {
        if (name == c.name) {
            return &c;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("[adr0043] dmath gives the same bits on every platform") {
    for (const DmathCase& c : dmath_cases) {
        dv::Hash hash, hash_f;
        for (std::uint64_t i = 0; i < dmath_inputs; ++i) {
            const dv::Input in = dv::input(*c.domain, i);
            hash.add(c.d(in.x, in.y));
            const dv::InputF in_f = dv::input_f(*c.domain, i);
            hash_f.add(c.f(in_f.x, in_f.y));
        }
        INFO(std::format("dmath::{}: double 0x{:016x}, float 0x{:016x}", c.name, hash.value, hash_f.value));
        CHECK(hash.value == c.hash);
        CHECK(hash_f.value == c.hash_f);
    }
}

TEST_CASE("[adr0043] dmath is correctly rounded on hard-to-round inputs") {
    for (const HardCase& h : hard_cases) {
        const DmathCase* c = dmath_case(h.name);
        REQUIRE(c != nullptr);
        const double r = c->d(h.x, h.y);
        INFO(std::format("dmath::{}({:a}, {:a}) = {:a}, expected {:a}{}", h.name, h.x, h.y, r,
                         std::bit_cast<double>(h.expected), h.correctly_rounded ? "" : " (dmath v1, not CR)"));
        CHECK(std::bit_cast<std::uint64_t>(r) == h.expected);
    }
    for (const HardCaseF& h : hard_cases_f) {
        std::string_view name = h.name;
        name.remove_suffix(1);
        const DmathCase* c = dmath_case(name);
        REQUIRE(c != nullptr);
        const float r = c->f(h.x, h.y);
        INFO(std::format("dmath::{}({:a}, {:a}) = {:a}, expected {:a}", name, h.x, h.y, r,
                         std::bit_cast<float>(h.expected)));
        CHECK(std::bit_cast<std::uint32_t>(r) == h.expected);
    }
}
