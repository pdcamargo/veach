#pragma once

// veach::hash: counter-based randomness for generation and simulation LOD
// (docs/adr/0044-counter-based-hash-randomness.md). Stateless: the same key words give the same bits on every
// platform, in any order, on any thread. Integer operations only, so everything here is canonical
// (docs/adr/0043-determinism-tiers-and-strict-floating-point.md).
//
// Composition (fixed by the test vectors in tests/space_gen_tests.cpp; changing any result is a determinism
// contract change, docs/adr/0055-universe-identity-and-versioning.md):
//
//   h = mix64(seed ^ salt)
//   for each key word w: h = mix64(h ^ mix64(w + golden))
//
// mix64 is the SplitMix64 finalizer (Steele, Lea and Flood 2014, Stafford's variant 13), golden the 64-bit
// golden-ratio increment and salt the first SHA-512 initial value (fractional bits of sqrt 2). Each word goes through
// its own finalizer before it meets the chain, so neighboring words (index i and i + 1) enter as unrelated values.
// The seed is salted differently from the words: with the same transform on both, hash(a, b) == hash(b, a) and
// hash(0, 0) == 0.
//
// Domains and salts are always hash_id() of stable names, never list positions, load order or enum values.

#include <cstdint>
#include <string_view>

namespace veach {

inline constexpr std::uint64_t hash_golden = 0x9E3779B97F4A7C15ULL;
inline constexpr std::uint64_t hash_seed_salt = 0x6A09E667F3BCC908ULL;

/// The SplitMix64 finalizer: a bijection on 64 bits with full avalanche.
constexpr std::uint64_t mix64(std::uint64_t z) {
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ULL;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return z;
}

namespace detail {
constexpr std::uint64_t hash_absorb(std::uint64_t h, std::uint64_t word) {
    return mix64(h ^ mix64(word + hash_golden));
}
}  // namespace detail

/// Hash of a seed and 1 to 6 key words. ADR 0044's layout: (seed, domain, id, index).
template <typename... Words>
    requires(sizeof...(Words) >= 1 && sizeof...(Words) <= 6)
constexpr std::uint64_t hash(std::uint64_t seed, Words... words) {
    std::uint64_t h = mix64(seed ^ hash_seed_salt);
    ((h = detail::hash_absorb(h, static_cast<std::uint64_t>(words))), ...);
    return h;
}

/// 64-bit FNV-1a over the UTF-8 bytes of a stable string id ("space:system", "orbits").
constexpr std::uint64_t hash_id(std::string_view id) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : id) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001B3ULL;
    }
    return h;
}

/// Uniform double in [0, 1) from the top 53 bits: exact, every value a multiple of 2^-53.
constexpr double unit_double(std::uint64_t h) {
    return static_cast<double>(h >> 11) * 0x1p-53;
}

/// Uniform float in [0, 1) from the top 24 bits.
constexpr float unit_float(std::uint64_t h) {
    return static_cast<float>(h >> 40) * 0x1p-24f;
}

/// High 64 bits of a 64 × 64 bit product, portable (MSVC has no __int128) and constexpr.
constexpr std::uint64_t mul_high64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a_lo = a & 0xFFFFFFFFULL, a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xFFFFFFFFULL, b_hi = b >> 32;
    const std::uint64_t lo_lo = a_lo * b_lo;
    const std::uint64_t hi_lo = a_hi * b_lo;
    const std::uint64_t lo_hi = a_lo * b_hi;
    const std::uint64_t hi_hi = a_hi * b_hi;
    const std::uint64_t cross = (lo_lo >> 32) + (hi_lo & 0xFFFFFFFFULL) + lo_hi;
    return hi_hi + (hi_lo >> 32) + (cross >> 32);
}

/// Uniform integer in [lo, hi] (inclusive) by multiply-high: bias below 2^-32 for spans under 2^32.
constexpr std::int64_t range(std::uint64_t h, std::int64_t lo, std::int64_t hi) {
    const std::uint64_t span = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo) + 1u;
    if (span == 0) {  // the full 64-bit range
        return static_cast<std::int64_t>(h);
    }
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) + mul_high64(h, span));
}

/// A readable sequence that stays counter-based: next() is hash(key, counter++). Each named sub-stream of a stage
/// has its own key, so a new draw in one never moves another. Plain state, reflectable.
struct HashStream {
    std::uint64_t key = 0;
    std::uint64_t counter = 0;

    /// key = hash(stage_seed, hash_id(substream), address).
    static constexpr HashStream make(std::uint64_t stage_seed, std::string_view substream, std::uint64_t address) {
        return HashStream{hash(stage_seed, hash_id(substream), address), 0};
    }

    constexpr std::uint64_t next() { return hash(key, counter++); }
    constexpr double next_double() { return unit_double(next()); }
    /// Uniform in [lo, hi).
    constexpr double uniform(double lo, double hi) { return lo + (hi - lo) * next_double(); }
    constexpr std::int64_t range(std::int64_t lo, std::int64_t hi) { return veach::range(next(), lo, hi); }
    /// True with probability p.
    constexpr bool chance(double p) { return next_double() < p; }
};

}  // namespace veach
