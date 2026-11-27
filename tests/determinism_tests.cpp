// Canonical floating point (docs/adr/0043-determinism-tiers-and-strict-floating-point.md, VCH-077): the same
// source gives the same bits on macOS arm64 (Apple Clang) and Windows x64 (MSVC). The computation below is full of
// `a * b + c`, which a compiler that contracts to FMA rounds differently; the committed hash holds only when every
// CPU target is built with contraction off (cmake/VeachCompiler.cmake).

#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <format>

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
