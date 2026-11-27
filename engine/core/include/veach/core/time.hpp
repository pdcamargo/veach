#pragma once

// Simulation time. The simulation advances in fixed steps of 1/60 s (ADR 0013).
// Wall-clock time only decides *how many* fixed steps run in a frame; it never
// reaches simulation code. Headless runs use exactly one step per frame, which
// makes them deterministic regardless of machine speed.

#include <cstdint>

namespace veach {

inline constexpr double fixed_timestep_hz = 60.0;
inline constexpr double fixed_delta_seconds = 1.0 / fixed_timestep_hz;

/// Turns variable frame times into a whole number of fixed steps.
class FixedTimestep {
public:
    explicit FixedTimestep(double step_seconds = fixed_delta_seconds, std::uint32_t max_steps_per_frame = 8)
        : step_(step_seconds), max_steps_(max_steps_per_frame) {}

    /// Adds real elapsed time and returns how many fixed steps to run now.
    /// Clamped to max_steps_per_frame so a long stall cannot spiral.
    std::uint32_t advance(double real_seconds) {
        accumulator_ += real_seconds;
        std::uint32_t steps = 0;
        while (accumulator_ >= step_ && steps < max_steps_) {
            accumulator_ -= step_;
            ++steps;
        }
        if (steps == max_steps_ && accumulator_ >= step_) {
            accumulator_ = 0.0;  // drop the backlog instead of falling further behind
        }
        return steps;
    }

    /// Fraction of a step left in the accumulator, for render interpolation.
    [[nodiscard]] double alpha() const { return accumulator_ / step_; }
    [[nodiscard]] double step() const { return step_; }

private:
    double step_;
    std::uint32_t max_steps_;
    double accumulator_ = 0.0;
};

}  // namespace veach
