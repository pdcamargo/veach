#pragma once

// Deterministic random numbers (PCG32). Simulation code must use an Rng seeded
// from the engine seed (--seed), never std::random_device or rand().
// The state is public plain data so it can be reflected, serialized and replayed.

#include <cstdint>

namespace veach {

struct Rng {
    explicit constexpr Rng(std::uint64_t seed = 0, std::uint64_t stream = 0x5851F42D4C957F2DULL) {
        reseed(seed, stream);
    }

    constexpr void reseed(std::uint64_t seed, std::uint64_t stream = 0x5851F42D4C957F2DULL) {
        state = 0;
        increment = (stream << 1u) | 1u;
        next_u32();
        state += seed;
        next_u32();
    }

    constexpr std::uint32_t next_u32() {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ULL + increment;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    /// Uniform in [0, 1).
    constexpr float next_float() { return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f); }

    /// Uniform integer in [lo, hi] (inclusive). Slight modulo bias is acceptable for gameplay.
    constexpr std::int32_t range(std::int32_t lo, std::int32_t hi) {
        const auto span = static_cast<std::uint32_t>(hi - lo) + 1u;
        return lo + static_cast<std::int32_t>(span == 0 ? next_u32() : next_u32() % span);
    }

    std::uint64_t state = 0;
    std::uint64_t increment = 0;
};

}  // namespace veach
