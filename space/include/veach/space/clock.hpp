#pragma once

// veach: canonical
// The universe clock and exact orbital phase (docs/adr/0047-universe-clock.md). Spike SP-04 (VCH-080),
// docs/research/spikes/sp-04-orrery-clock.md.
//
// Time is an int64 tick at the 60 Hz fixed step. Every period is stored as period_q, an int64 in units of 2^-16 tick,
// and a phase is integer arithmetic plus one IEEE division, so it is the same on every platform and periods in integer
// ratios stay locked forever.

#include <cstdint>

namespace veach::space {

inline constexpr std::int64_t ticks_per_second = 60;
inline constexpr std::int64_t ticks_per_day = 86'400 * ticks_per_second;
/// period_q is in units of 2^-16 tick.
inline constexpr int period_q_bits = 16;
inline constexpr std::int64_t period_q_per_tick = std::int64_t{1} << period_q_bits;
/// Ticks stay below 2^53 (4.76 million years) so Luau, JSON and MCP carry them as exact numbers.
inline constexpr std::int64_t tick_limit = std::int64_t{1} << 53;

/// The universe clock. Reflected and Scriptable by OrreryModule (veach/space/orrery.hpp).
struct UniverseClock {
    std::int64_t tick = 0;  ///< universe ticks since the universe epoch, in [0, 2^53)
    std::int64_t rate = 1;  ///< ticks advanced per fixed step: 1 normally, W under rails warp, 0 paused
};

/// A period in seconds as period_q, rounded to nearest. `seconds` must be positive and below about 74,000 years.
[[nodiscard]] std::int64_t period_q_from_seconds(double seconds);

/// A period_q back in seconds (one IEEE division).
[[nodiscard]] double seconds_from_period_q(std::int64_t period_q);

/// ((tick − epoch_tick) × 2^16) mod period_q, exactly, in [0, period_q). Any sign of tick − epoch_tick; any
/// period_q ≥ 1 (the whole int64 range: shift-and-subtract, no 128-bit product).
[[nodiscard]] std::uint64_t phase_residue(std::int64_t tick, std::int64_t epoch_tick, std::int64_t period_q);

/// The phase in [0, 1): phase_residue / period_q, one IEEE division.
[[nodiscard]] double phase(std::int64_t tick, std::int64_t epoch_tick, std::int64_t period_q);

}  // namespace veach::space
