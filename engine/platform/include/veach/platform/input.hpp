#pragma once

// Input as data. Platform events become the Input singleton (raw keys) and
// InputAction entities (named, rebindable actions). Gameplay reads actions:
//
//   veach::bind_action(world, "jump", veach::Key::space);
//   if (veach::action_pressed(world, "jump")) { ... }
//
// Raw SDL events never leave engine/platform.

#include <veach/ecs/flecs.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace veach {

// clang-format off
enum class Key : std::uint8_t {
    unknown,
    a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,
    digit0, digit1, digit2, digit3, digit4, digit5, digit6, digit7, digit8, digit9,
    space, enter, escape, tab, backspace,
    left, right, up, down,
    left_shift, right_shift, left_ctrl, right_ctrl, left_alt, right_alt,
    f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
};
// clang-format on
inline constexpr std::size_t key_count = static_cast<std::size_t>(Key::f12) + 1;

/// Singleton. Keyboard state for the current frame.
struct Input {
    std::uint64_t down[2] = {};      ///< Held this frame.
    std::uint64_t pressed[2] = {};   ///< Went down this frame.
    std::uint64_t released[2] = {};  ///< Went up this frame.

    [[nodiscard]] bool is_down(Key key) const { return test(down, key); }
    [[nodiscard]] bool just_pressed(Key key) const { return test(pressed, key); }
    [[nodiscard]] bool just_released(Key key) const { return test(released, key); }

    void begin_frame() {
        pressed[0] = pressed[1] = 0;
        released[0] = released[1] = 0;
    }
    void set_key(Key key, bool is_down);

private:
    static bool test(const std::uint64_t (&bits)[2], Key key) {
        const auto index = static_cast<std::size_t>(key);
        return ((bits[index / 64] >> (index % 64)) & 1u) != 0;
    }
};

/// A named action, one entity per action under the `veach.actions` scope.
/// State fields are written by the platform module every frame.
struct InputAction {
    Key primary = Key::unknown;
    Key secondary = Key::unknown;
    bool down = false;
    bool pressed = false;
    bool released = false;
};

/// Creates or rebinds the action entity `name`.
Entity bind_action(World& world, std::string_view name, Key primary, Key secondary = Key::unknown);
[[nodiscard]] bool action_down(const World& world, std::string_view name);
[[nodiscard]] bool action_pressed(const World& world, std::string_view name);
[[nodiscard]] bool action_released(const World& world, std::string_view name);

}  // namespace veach
