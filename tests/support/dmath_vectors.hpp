#pragma once

// Deterministic input sets and output hashes for veach::dmath (docs/research/spikes/sp-02-dmath.md, VCH-078).
// Shared by the unit test (tests/determinism_tests.cpp, 10^5 inputs per function) and the benchmark
// (tests/bench/dmath_bench.cpp, 10^7). Inputs are built from integer hashes and exact bit patterns only, so every
// platform sees the same doubles; a hash differs only if a function returns different bits.

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>

namespace veach::test::dmath_vectors {

/// SplitMix64 finalizer: deterministic everywhere, no floating point.
constexpr std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/// Uniform in [lo, hi): 53 random bits scaled with basic operations only.
constexpr double uniform(std::uint64_t h, double lo, double hi) {
    return lo + (hi - lo) * (static_cast<double>(h >> 11) * 0x1p-53);
}

/// Any finite double or subnormal, exponent field uniform in [0, 2046]: covers tiny, huge and subnormal inputs.
constexpr double any_finite(std::uint64_t h, bool positive_only) {
    const std::uint64_t sign = positive_only ? 0 : (h & 1u) << 63;
    const std::uint64_t exponent = ((h >> 1) % 2047u) << 52;
    const std::uint64_t mantissa = mix(h) & 0x000fffffffffffffULL;
    return std::bit_cast<double>(sign | exponent | mantissa);
}

/// One input pair; `y` is unused by one-argument functions.
struct Input {
    double x;
    double y;
};

/// The input domain of one function: a typical range (half the inputs), then anything finite, then raw bits.
struct Domain {
    std::uint64_t key;  ///< Separates the input streams of different functions.
    double x_lo, x_hi;
    double y_lo, y_hi;
    bool x_positive;               ///< Wide inputs stay positive (log of a negative number is just NaN).
    bool integer_y;                ///< A quarter of the typical y values are integers (pow's exact cases).
    std::span<const Input> edges;  ///< Tried first, before the generated inputs.
};

constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double min_sub = std::numeric_limits<double>::denorm_min();
constexpr double max_sub = 0x0.fffffffffffffp-1022;
constexpr double min_norm = std::numeric_limits<double>::min();
constexpr double max_fin = std::numeric_limits<double>::max();

/// Special values for every one-argument function: signed zeros, infinities, NaN, subnormals, the ends of the range,
/// exact small values and large arguments (sin and cos reduce these with Payne-Hanek).
inline constexpr std::array<Input, 34> unary_edges{{
    {0.0, 0},
    {-0.0, 0},
    {inf, 0},
    {-inf, 0},
    {nan, 0},
    {-nan, 0},
    {min_sub, 0},
    {-min_sub, 0},
    {max_sub, 0},
    {-max_sub, 0},
    {min_norm, 0},
    {-min_norm, 0},
    {max_fin, 0},
    {-max_fin, 0},
    {1.0, 0},
    {-1.0, 0},
    {0.5, 0},
    {2.0, 0},
    {10.0, 0},
    {100.0, 0},
    {0x1p-30, 0},
    {0x1p-60, 0},
    {0x1p+30, 0},
    {0x1p+60, 0},
    {0x1p+1023, 0},
    {1e22, 0},
    {-1e22, 0},
    {0x1.921fb54442d18p+0, 0},  // nearest double to pi/2
    {0x1.921fb54442d18p+1, 0},  // nearest double to pi
    {0x1.921fb54442d18p+2, 0},  // nearest double to 2 pi
    {709.782712893384, 0},
    {-745.1332191019411, 0},
    {-708.3964185322641, 0},    // exp overflow and underflow edges
    {0x1.62e42fefa39efp-1, 0},  // nearest double to ln 2
}};

/// pow's IEEE 754 special cases (C23 F.10.4.5) and exact results.
inline constexpr std::array<Input, 44> pow_edges{{
    {0.0, -1.0},    {-0.0, -1.0},   {-0.0, -3.0},      {0.0, -inf},
    {-0.0, -inf},   {0.0, 3.0},     {-0.0, 3.0},       {-0.0, 0.5},
    {-1.0, inf},    {-1.0, -inf},   {1.0, nan},        {1.0, inf},
    {nan, 0.0},     {nan, -0.0},    {2.0, nan},        {nan, 2.0},
    {0.5, inf},     {0.5, -inf},    {2.0, inf},        {2.0, -inf},
    {-inf, -3.0},   {-inf, -2.0},   {-inf, 3.0},       {-inf, 2.0},
    {inf, -0.5},    {inf, 0.5},     {-8.0, 1.0 / 3.0}, {-2.0, 0.5},
    {-2.0, 3.0},    {-2.0, -3.0},   {2.0, 10.0},       {2.0, -1074.0},
    {2.0, 1023.0},  {2.0, 1024.0},  {10.0, 22.0},      {10.0, 23.0},
    {10.0, -5.0},   {9.0, 0.5},     {16.0, 0.25},      {0.0, 0.0},
    {min_sub, 0.5}, {max_fin, 2.0}, {max_fin, -2.0},   {1.0000000000000002, 1e16},
}};

/// atan2's signed zeros and infinities (C23 F.10.1.4) and ratios that are exact or extreme.
inline constexpr std::array<Input, 28> atan2_edges{{
    {0.0, 0.0},         {0.0, -0.0},         {-0.0, 0.0},        {-0.0, -0.0},    {0.0, 1.0},   {0.0, -1.0},
    {-0.0, -1.0},       {1.0, 0.0},          {-1.0, 0.0},        {1.0, -0.0},     {inf, inf},   {inf, -inf},
    {-inf, inf},        {-inf, -inf},        {1.0, inf},         {1.0, -inf},     {-1.0, -inf}, {inf, 1.0},
    {-inf, 1.0},        {nan, 1.0},          {1.0, nan},         {1.0, 1.0},      {-1.0, -1.0}, {max_fin, min_sub},
    {min_sub, max_fin}, {min_sub, -max_fin}, {max_sub, max_sub}, {1e-300, 1e300},
}};

/// The i-th input of `domain`. Index 0 onward is the edge list, then generated inputs.
constexpr Input input(const Domain& domain, std::uint64_t i) {
    if (i < domain.edges.size()) {
        return domain.edges[static_cast<std::size_t>(i)];
    }
    const std::uint64_t h = mix(domain.key ^ mix(i));
    const std::uint64_t hy = mix(h ^ 0x5bd1e995ULL);
    switch (i % 8) {
        case 0:
        case 1:
        case 2:
        case 3: {
            double y = uniform(hy, domain.y_lo, domain.y_hi);
            if (domain.integer_y && i % 8 == 3) {
                y = static_cast<double>(static_cast<std::int64_t>(y));
            }
            return {uniform(h, domain.x_lo, domain.x_hi), y};
        }
        case 4:
        case 5:
        case 6: return {any_finite(h, domain.x_positive), any_finite(hy, false)};
        default: return {std::bit_cast<double>(h), std::bit_cast<double>(hy)};  // anything: NaN and inf included
    }
}

/// FNV-1a over the result bits. Every NaN hashes the same: NaN payloads and signs are not portable (x86 makes a
/// negative default NaN, Arm a positive one, even for 0.0 / 0.0), so canonical code may rely on "is NaN", never on
/// its bits.
struct Hash {
    std::uint64_t value = 14695981039346656037ULL;

    constexpr void add(std::uint64_t bits) {
        for (int byte = 0; byte < 8; ++byte) {
            value = (value ^ ((bits >> (8 * byte)) & 0xffu)) * 1099511628211ULL;
        }
    }
    constexpr void add(double r) { add(r != r ? 0x7ff8000000000000ULL : std::bit_cast<std::uint64_t>(r)); }
    constexpr void add(float r) {
        add(static_cast<std::uint64_t>(r != r ? 0x7fc00000u : std::bit_cast<std::uint32_t>(r)));
    }
};

/// A double narrowed to float, saturating to infinity (an out-of-range conversion is undefined behavior in C++).
constexpr float narrow(double v) {
    if (v > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::numeric_limits<float>::infinity();
    }
    if (v < -static_cast<double>(std::numeric_limits<float>::max())) {
        return -std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(v);
}

/// One float input pair.
struct InputF {
    float x;
    float y;
};

/// The i-th float input of `domain`: the edges narrowed, then the same mix as `input`, built from float bits.
constexpr InputF input_f(const Domain& domain, std::uint64_t i) {
    if (i < domain.edges.size()) {
        const Input e = domain.edges[static_cast<std::size_t>(i)];
        return {narrow(e.x), narrow(e.y)};
    }
    const std::uint64_t h = mix(domain.key ^ mix(i) ^ 0xf10a7ULL);
    const std::uint64_t hy = mix(h ^ 0x5bd1e995ULL);
    const auto any_finite_f = [](std::uint64_t bits, bool positive_only) {
        const std::uint32_t sign = positive_only ? 0u : static_cast<std::uint32_t>(bits & 1u) << 31;
        const auto exponent = static_cast<std::uint32_t>((bits >> 1) % 255u) << 23;
        const auto mantissa = static_cast<std::uint32_t>(mix(bits)) & 0x007fffffu;
        return std::bit_cast<float>(sign | exponent | mantissa);
    };
    switch (i % 8) {
        case 0:
        case 1:
        case 2:
        case 3: {
            float y = narrow(uniform(hy, domain.y_lo, domain.y_hi));
            if (domain.integer_y && i % 8 == 3) {
                y = static_cast<float>(static_cast<std::int64_t>(y));
            }
            return {narrow(uniform(h, domain.x_lo, domain.x_hi)), y};
        }
        case 4:
        case 5:
        case 6: return {any_finite_f(h, domain.x_positive), any_finite_f(hy, false)};
        default:
            return {std::bit_cast<float>(static_cast<std::uint32_t>(h)),
                    std::bit_cast<float>(static_cast<std::uint32_t>(hy))};
    }
}

/// The input domains, keyed by function name; float functions use the same domain as their double version.
/// Binary functions take (x, y) in order: atan2(x, y) is atan2 of "y = x" over "x = y" in C's naming.
namespace domains {
inline constexpr Domain sin{1, -8.0, 8.0, 0, 0, false, false, unary_edges};
inline constexpr Domain cos{2, -8.0, 8.0, 0, 0, false, false, unary_edges};
inline constexpr Domain tan{3, -8.0, 8.0, 0, 0, false, false, unary_edges};
inline constexpr Domain asin{4, -1.0, 1.0, 0, 0, false, false, unary_edges};
inline constexpr Domain acos{5, -1.0, 1.0, 0, 0, false, false, unary_edges};
inline constexpr Domain atan{6, -20.0, 20.0, 0, 0, false, false, unary_edges};
inline constexpr Domain atan2{7, -10.0, 10.0, -10.0, 10.0, false, false, atan2_edges};
inline constexpr Domain exp{8, -20.0, 20.0, 0, 0, false, false, unary_edges};
inline constexpr Domain exp2{9, -30.0, 30.0, 0, 0, false, false, unary_edges};
inline constexpr Domain log{10, 0.0, 100.0, 0, 0, true, false, unary_edges};
inline constexpr Domain log2{11, 0.0, 100.0, 0, 0, true, false, unary_edges};
inline constexpr Domain log10{12, 0.0, 100.0, 0, 0, true, false, unary_edges};
inline constexpr Domain pow{13, 0.0, 4.0, -30.0, 30.0, false, true, pow_edges};
inline constexpr Domain cbrt{14, -100.0, 100.0, 0, 0, false, false, unary_edges};
inline constexpr Domain hypot{15, -10.0, 10.0, -10.0, 10.0, false, false, atan2_edges};
inline constexpr Domain sinh{16, -10.0, 10.0, 0, 0, false, false, unary_edges};
inline constexpr Domain cosh{17, -10.0, 10.0, 0, 0, false, false, unary_edges};
inline constexpr Domain tanh{18, -10.0, 10.0, 0, 0, false, false, unary_edges};
}  // namespace domains

}  // namespace veach::test::dmath_vectors
