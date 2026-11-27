// veach: canonical (ADR 0043): only integer arithmetic and IEEE + − × ÷.
#include <veach/space/clock.hpp>

#include <cmath>

namespace veach::space {

std::int64_t period_q_from_seconds(double seconds) {
    return std::llround(seconds * static_cast<double>(ticks_per_second * period_q_per_tick));
}

double seconds_from_period_q(std::int64_t period_q) {
    return static_cast<double>(period_q) / static_cast<double>(ticks_per_second * period_q_per_tick);
}

std::uint64_t phase_residue(std::int64_t tick, std::int64_t epoch_tick, std::int64_t period_q) {
    // Δtick mod p, non-negative. |Δtick| < 2^54, so the subtraction can't overflow.
    std::int64_t m = (tick - epoch_tick) % period_q;
    if (m < 0) {
        m += period_q;
    }
    // (m × 2^16) mod p, one bit at a time: r < p ≤ 2^63, so 2r < 2^64 fits in uint64. ADR 0047's
    // ((Δtick mod p) × 2^16) mod p in int64 overflows as soon as p ≥ 2^47, i.e. for periods over 1.13 years.
    const auto p = static_cast<std::uint64_t>(period_q);
    auto r = static_cast<std::uint64_t>(m);
    for (int i = 0; i < period_q_bits; ++i) {
        r <<= 1;
        if (r >= p) {
            r -= p;
        }
    }
    return r;
}

double phase(std::int64_t tick, std::int64_t epoch_tick, std::int64_t period_q) {
    const double f = static_cast<double>(phase_residue(tick, epoch_tick, period_q)) / static_cast<double>(period_q);
    // Above 2^53 the conversion can round r up to p; 1.0 is the same angle as 0.
    return f < 1.0 ? f : 0.0;
}

}  // namespace veach::space
