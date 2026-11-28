#pragma once

// Input as data. Platform events become the Input singleton (raw keys and analog axes) and
// InputAction / InputAxis entities (named, rebindable actions). Gameplay reads actions:
//
//   veach::bind_action(world, "jump", veach::Key::space);
//   if (veach::action_pressed(world, "jump")) { ... }
//   veach::bind_axis(world, "look_x", {.source = veach::Axis::mouse_x, .sensitivity = 0.003f});
//   const float yaw = veach::axis_value(world, "look_x");
//
// Raw SDL events never leave engine/platform. Analog values travel as fixed point (1/65536 steps,
// axis_one) from the event on, so recordings replay them bit for bit.

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
    mouse_left, mouse_right, mouse_middle,  ///< mouse buttons bind like keys
    // Gamepad buttons (any connected gamepad), by position: south is A on Xbox, cross on PlayStation.
    gamepad_south, gamepad_east, gamepad_west, gamepad_north,
    gamepad_back, gamepad_guide, gamepad_start, gamepad_left_stick, gamepad_right_stick,
    gamepad_left_shoulder, gamepad_right_shoulder,
    gamepad_dpad_up, gamepad_dpad_down, gamepad_dpad_left, gamepad_dpad_right,
};
// clang-format on
inline constexpr std::size_t key_count = static_cast<std::size_t>(Key::gamepad_dpad_right) + 1;
static_assert(key_count <= 128, "Input keeps two 64-bit words per state");

/// A raw analog input. Mouse axes are relative (motion since the last fixed step, in window points or wheel
/// notches); gamepad axes are absolute (sticks -1..1, triggers 0..1, any connected gamepad). +Y is up for the
/// mouse and the sticks (ADR 0013), so pushing a stick away or moving the mouse up reads positive.
enum class Axis : std::uint8_t {
    none,
    mouse_x,
    mouse_y,
    mouse_wheel_x,
    mouse_wheel_y,
    gamepad_left_x,
    gamepad_left_y,
    gamepad_right_x,
    gamepad_right_y,
    gamepad_left_trigger,
    gamepad_right_trigger,
};
inline constexpr std::size_t axis_count = static_cast<std::size_t>(Axis::gamepad_right_trigger) + 1;

/// Analog values are fixed point: 1.0 is axis_one. Events carry them quantized, so a recording replays exactly.
inline constexpr std::int32_t axis_one = 65536;
/// `value` rounded to the 1/65536 grid (half away from zero), saturated to the int32 range.
[[nodiscard]] std::int32_t quantize_axis(double value);
[[nodiscard]] constexpr double axis_to_double(std::int32_t fixed) {
    return static_cast<double>(fixed) / axis_one;
}
/// Relative axes accumulate motion; absolute axes hold a position.
[[nodiscard]] constexpr bool is_relative(Axis axis) {
    return axis >= Axis::mouse_x && axis <= Axis::mouse_wheel_y;
}

/// Singleton. Keyboard, mouse-button, gamepad and analog state for the current frame.
struct Input {
    std::uint64_t down[2] = {};      ///< Held this frame.
    std::uint64_t pressed[2] = {};   ///< Went down this frame.
    std::uint64_t released[2] = {};  ///< Went up this frame.
    /// Fixed point (axis_one = 1.0), indexed by Axis. Relative axes: motion since the last fixed step.
    std::int32_t axes[axis_count] = {};
    std::uint32_t gamepads = 0;  ///< Connected gamepads.

    [[nodiscard]] bool is_down(Key key) const { return test(down, key); }
    [[nodiscard]] bool just_pressed(Key key) const { return test(pressed, key); }
    [[nodiscard]] bool just_released(Key key) const { return test(released, key); }
    [[nodiscard]] std::int32_t axis(Axis a) const { return axes[static_cast<std::size_t>(a)]; }

    void begin_frame() {
        pressed[0] = pressed[1] = 0;
        released[0] = released[1] = 0;
    }
    void set_key(Key key, bool is_down);
    /// Relative axes add `fixed` (saturating); absolute axes take it. Axis::none is ignored.
    void move_axis(Axis a, std::int32_t fixed);

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

/// How a named analog action reads its inputs ("axes" in veach.project.json). Its value is
///   (sensitivity * dead_zoned(source) + down(positive) - down(negative)) * (invert ? -1 : 1),
/// rounded to the 1/65536 grid. Keys are the digital fallback: they add exactly 1, unscaled.
struct AxisBinding {
    Axis source = Axis::none;
    Key negative = Key::unknown;
    Key positive = Key::unknown;
    /// |source| below this reads 0; above, the rest is rescaled to start at 0 (no jump at the edge).
    float dead_zone = 0.0f;
    float sensitivity = 1.0f;  ///< Multiplies the source (per window point for the mouse).
    bool invert = false;       ///< Negates the whole value.
};

/// A named analog action, an entity under `veach.actions` like InputAction. `value` is written by the platform
/// module in the Input phase, and again after each frame's first fixed step consumes the relative axes.
struct InputAxis {
    AxisBinding binding;
    float value = 0.0f;
};

/// The path of action `name`'s entity: veach::actions::<name>.
[[nodiscard]] std::string action_path(std::string_view name);

/// Creates or rebinds the action entity `name`.
Entity bind_action(World& world, std::string_view name, Key primary, Key secondary = Key::unknown);
[[nodiscard]] bool action_down(const World& world, std::string_view name);
[[nodiscard]] bool action_pressed(const World& world, std::string_view name);
[[nodiscard]] bool action_released(const World& world, std::string_view name);

/// Creates or rebinds the analog action entity `name`.
Entity bind_axis(World& world, std::string_view name, const AxisBinding& binding);
/// The axis action's value this frame, 0 if `name` is not bound.
[[nodiscard]] float axis_value(const World& world, std::string_view name);
/// `binding` applied to `input`: what InputAxis::value holds (exposed for tests).
[[nodiscard]] float evaluate_axis(const AxisBinding& binding, const Input& input);

}  // namespace veach
