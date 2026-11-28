// Spike SP-13 (VCH-089), throwaway: the SP-13 scene as an engine module, so a landing goes through the real input
// path and the engine's --record/--replay (docs/specs/record-replay/spec.md). Six analog actions on the gamepad
// carry the controls; a Startup script sets Sp13Run on an entity named "sp13"; each fixed step (Simulation) reads the
// axes, steps the Sim and publishes Sp13Craft, which the replay's per-frame state hash covers.
#pragma once

#include "sp13/sp13_sim.hpp"

#include <veach/veach.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace veach::sp13 {

/// Which scene to build: set once, from the startup script.
struct Sp13Run {
    double scale = 0.01;
    bool craft = true;
    bool walkers = true;
};

/// The craft's CraftView and the Sim's state hash after the last step (hash split in two 32-bit halves so JSON and
/// Luau carry it exactly).
struct Sp13Craft {
    std::int64_t tick = 0;
    double altitude = 0.0;
    double ve = 0.0, vn = 0.0, vu = 0.0;
    double offset_e = 0.0, offset_n = 0.0;
    double cap = 0.0;
    std::int32_t frame = 0;
    std::int32_t scene = 0;
    std::int64_t hash_hi = 0;
    std::int64_t hash_lo = 0;
};

struct Sp13Module {
    explicit Sp13Module(World& world);
};

/// The analog actions, in Controls order (lift, strafe_x, strafe_z, engine, walk, turn), and their gamepad axes.
inline constexpr std::array<std::string_view, 6> action_names = {"sp13_lift",   "sp13_strafe_x", "sp13_strafe_z",
                                                                 "sp13_engine", "sp13_walk",     "sp13_turn"};
inline constexpr std::array<Axis, 6> action_axes = {Axis::gamepad_right_y,      Axis::gamepad_left_x,
                                                    Axis::gamepad_left_y,       Axis::gamepad_right_trigger,
                                                    Axis::gamepad_left_trigger, Axis::gamepad_right_x};

/// The controls in action order.
[[nodiscard]] std::array<std::int32_t, 6> control_values(const Controls& c);
[[nodiscard]] CraftView view_of(const Sp13Craft& craft);
[[nodiscard]] std::uint64_t hash_of(const Sp13Craft& craft);

}  // namespace veach::sp13
