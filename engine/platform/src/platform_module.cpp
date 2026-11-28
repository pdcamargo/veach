#include <veach/platform/platform_module.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/services.hpp>
#include <veach/platform/input.hpp>
#include <veach/platform/platform.hpp>
#include <veach/platform/window.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace veach {

namespace {

constexpr const char* actions_scope = "veach::actions";

bool is_applied(PlatformEvent::Type type) {
    switch (type) {
        case PlatformEvent::Type::quit_requested:
        case PlatformEvent::Type::window_close_requested: return false;
        case PlatformEvent::Type::window_resized:
        case PlatformEvent::Type::key_down:
        case PlatformEvent::Type::key_up:
        case PlatformEvent::Type::axis_moved:
        case PlatformEvent::Type::gamepad_added:
        case PlatformEvent::Type::gamepad_removed: return true;
    }
    return false;
}

/// The last gamepad went away: its sticks and triggers read 0 and its held buttons are released.
void release_gamepad(Input& input) {
    for (std::size_t a = static_cast<std::size_t>(Axis::gamepad_left_x); a < axis_count; ++a) {
        input.axes[a] = 0;
    }
    for (std::size_t k = static_cast<std::size_t>(Key::gamepad_south); k < key_count; ++k) {
        input.set_key(static_cast<Key>(k), false);
    }
}

void update_actions(World& world, const Input& input) {
    world.each([&](InputAction& action) {
        action.down = input.is_down(action.primary) || input.is_down(action.secondary);
        action.pressed = input.just_pressed(action.primary) || input.just_pressed(action.secondary);
        action.released = input.just_released(action.primary) || input.just_released(action.secondary);
    });
    world.each([&](InputAxis& axis) { axis.value = evaluate_axis(axis.binding, input); });
}

void handle_events(World& world, Platform& platform) {
    std::vector<PlatformEvent> events;
    platform.poll_events(events);
    if (platform.replaying()) {
        std::erase_if(events, [](const PlatformEvent& e) {
            return e.type != PlatformEvent::Type::quit_requested &&
                   e.type != PlatformEvent::Type::window_close_requested;
        });
    }
    platform.take_injected(events);
    std::vector<PlatformEvent> applied;
    for (const PlatformEvent& e : events) {
        if (is_applied(e.type)) {
            applied.push_back(e);
        }
    }
    platform.set_applied_events(std::move(applied));

    Input& input = world.get_mut<Input>();
    input.begin_frame();

    for (const PlatformEvent& event : events) {
        switch (event.type) {
            case PlatformEvent::Type::quit_requested:
                log::info("platform", "quit requested by the OS");
                world.quit();
                break;
            case PlatformEvent::Type::window_close_requested:
                world.each([&](Entity e, const WindowPlatformState& state) {
                    if (state.id != event.window) {
                        return;
                    }
                    if (e.has<PrimaryWindow>()) {
                        log::info("platform", "primary window closed, quitting");
                        world.quit();
                    } else {
                        e.destruct();
                    }
                });
                break;
            case PlatformEvent::Type::window_resized:
                world.each([&](Entity e, WindowPlatformState& state) {
                    if (state.id == event.window) {
                        // Record as applied first, so SyncWindows does not echo it back to the OS.
                        state.applied.width = event.width;
                        state.applied.height = event.height;
                        if (const Window* current = e.try_get<Window>()) {
                            Window updated = *current;
                            updated.width = event.width;
                            updated.height = event.height;
                            e.set<Window>(std::move(updated));
                        }
                    }
                });
                break;
            case PlatformEvent::Type::key_down: input.set_key(event.key, true); break;
            case PlatformEvent::Type::key_up: input.set_key(event.key, false); break;
            case PlatformEvent::Type::axis_moved: input.move_axis(event.axis, event.value); break;
            case PlatformEvent::Type::gamepad_added:
                ++input.gamepads;
                log::info("platform", "gamepad connected ({} connected)", input.gamepads);
                break;
            case PlatformEvent::Type::gamepad_removed:
                if (input.gamepads > 0) {
                    --input.gamepads;
                }
                if (input.gamepads == 0) {
                    release_gamepad(input);
                }
                log::info("platform", "gamepad disconnected ({} connected)", input.gamepads);
                break;
        }
    }

    update_actions(world, input);
}

const InputAction* find_action(const World& world, std::string_view name) {
    const Entity e = world.lookup(action_path(name).c_str());
    return e ? e.try_get<InputAction>() : nullptr;
}

}  // namespace

std::int32_t quantize_axis(double value) {
    if (std::isnan(value)) {
        return 0;
    }
    const double scaled = value * axis_one;
    constexpr double lo = std::numeric_limits<std::int32_t>::min();
    constexpr double hi = std::numeric_limits<std::int32_t>::max();
    if (scaled <= lo) {
        return std::numeric_limits<std::int32_t>::min();
    }
    if (scaled >= hi) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(std::lround(scaled));  // half away from zero, whatever the rounding mode
}

void Input::move_axis(Axis a, std::int32_t fixed) {
    if (a == Axis::none) {
        return;
    }
    std::int32_t& slot = axes[static_cast<std::size_t>(a)];
    if (!is_relative(a)) {
        slot = fixed;
        return;
    }
    const std::int64_t sum = std::int64_t{slot} + fixed;
    slot = static_cast<std::int32_t>(std::clamp<std::int64_t>(sum, std::numeric_limits<std::int32_t>::min(),
                                                              std::numeric_limits<std::int32_t>::max()));
}

float evaluate_axis(const AxisBinding& binding, const Input& input) {
    double analog = binding.source == Axis::none ? 0.0 : axis_to_double(input.axis(binding.source));
    const double dead_zone = binding.dead_zone;
    if (dead_zone > 0.0) {
        const double magnitude = std::abs(analog);
        if (magnitude <= dead_zone) {
            analog = 0.0;
        } else {
            // Absolute axes rescale what is left to 0..1; relative ones (unbounded) just lose the dead zone.
            double rest = 0.0;
            if (is_relative(binding.source)) {
                rest = magnitude - dead_zone;
            } else if (dead_zone < 1.0) {
                rest = (magnitude - dead_zone) / (1.0 - dead_zone);
            }
            analog = analog < 0.0 ? -rest : rest;
        }
    }
    double value = analog * static_cast<double>(binding.sensitivity);
    value += (input.is_down(binding.positive) ? 1.0 : 0.0) - (input.is_down(binding.negative) ? 1.0 : 0.0);
    if (binding.invert) {
        value = -value;
    }
    return static_cast<float>(axis_to_double(quantize_axis(value)));
}

void consume_relative_axes(World& world) {
    Input& input = world.get_mut<Input>();
    bool moved = false;
    for (std::size_t a = 0; a < axis_count; ++a) {
        if (is_relative(static_cast<Axis>(a)) && input.axes[a] != 0) {
            input.axes[a] = 0;
            moved = true;
        }
    }
    if (moved) {
        world.each([&](InputAxis& axis) {
            if (is_relative(axis.binding.source)) {
                axis.value = evaluate_axis(axis.binding, input);
            }
        });
    }
}

void Input::set_key(Key key, bool is_down) {
    if (key == Key::unknown) {
        return;  // an untranslated key; recording it would trip every action whose unused slot is `unknown`
    }
    const auto index = static_cast<std::size_t>(key);
    const std::uint64_t bit = std::uint64_t{1} << (index % 64);
    std::uint64_t& word = down[index / 64];
    const bool was_down = (word & bit) != 0;
    if (is_down && !was_down) {
        pressed[index / 64] |= bit;
        word |= bit;
    } else if (!is_down && was_down) {
        released[index / 64] |= bit;
        word &= ~bit;
    }
}

std::string action_path(std::string_view name) {
    return std::string(actions_scope) + "::" + std::string(name);
}

Entity bind_action(World& world, std::string_view name, Key primary, Key secondary) {
    Entity e = world.entity(action_path(name).c_str());
    e.set<InputAction>({.primary = primary, .secondary = secondary});
    return e;
}

bool action_down(const World& world, std::string_view name) {
    const InputAction* action = find_action(world, name);
    return action && action->down;
}

bool action_pressed(const World& world, std::string_view name) {
    const InputAction* action = find_action(world, name);
    return action && action->pressed;
}

bool action_released(const World& world, std::string_view name) {
    const InputAction* action = find_action(world, name);
    return action && action->released;
}

Entity bind_axis(World& world, std::string_view name, const AxisBinding& binding) {
    Entity e = world.entity(action_path(name).c_str());
    e.set<InputAxis>({.binding = binding, .value = evaluate_axis(binding, world.get<Input>())});
    return e;
}

float axis_value(const World& world, std::string_view name) {
    const Entity e = world.lookup(action_path(name).c_str());
    const InputAxis* axis = e ? e.try_get<InputAxis>() : nullptr;
    return axis ? axis->value : 0.0f;
}

PlatformModule::PlatformModule(World& world) {
    world.module<PlatformModule>("veach::platform");

    world.component<WindowMode>();
    world.component<Window>()
        .member(&Window::title, "title")
        .member(&Window::width, "width")
        .member(&Window::height, "height")
        .member(&Window::mode, "mode")
        .member(&Window::vsync, "vsync")
        .add<Scriptable>();
    world.component<PrimaryWindow>();
    world.component<WindowPlatformState>()
        .member(&WindowPlatformState::id, "id")
        .member(&WindowPlatformState::applied, "applied")
        .member(&WindowPlatformState::relative_mouse, "relative_mouse");

    world.component<RelativeMouse>().member(&RelativeMouse::enabled, "enabled").add<Scriptable>();

    world.component<Key>();
    world.component<Axis>();
    world.component<Input>()
        .member(&Input::down, "down")
        .member(&Input::pressed, "pressed")
        .member(&Input::released, "released")
        .member(&Input::axes, "axes")
        .member(&Input::gamepads, "gamepads")
        .add(flecs::Singleton);
    world.component<InputAction>()
        .member(&InputAction::primary, "primary")
        .member(&InputAction::secondary, "secondary")
        .member(&InputAction::down, "down")
        .member(&InputAction::pressed, "pressed")
        .member(&InputAction::released, "released");
    world.component<AxisBinding>()
        .member(&AxisBinding::source, "source")
        .member(&AxisBinding::negative, "negative")
        .member(&AxisBinding::positive, "positive")
        .member(&AxisBinding::dead_zone, "dead_zone")
        .member(&AxisBinding::sensitivity, "sensitivity")
        .member(&AxisBinding::invert, "invert");
    world.component<InputAxis>().member(&InputAxis::binding, "binding").member(&InputAxis::value, "value");
    world.set<Input>({});

    // A window entity lost its Window component or was deleted: close the OS
    // window. id = 0 marks the state stale so a re-added Window gets a new one.
    world.observer<const Window>("CloseOsWindow").event(flecs::OnRemove).each([](Entity e, const Window&) {
        WindowPlatformState* state = e.try_get_mut<WindowPlatformState>();
        auto* platform = services(e.world()).find<Platform>();
        if (state && platform && state->id != 0) {
            platform->destroy_window(state->id);
            state->id = 0;
        }
    });

    world.system("PollPlatform").kind<phase::Input>().run([](Iter& it) {
        VEACH_ZONE("PollPlatform");
        World w = it.world();
        handle_events(w, services(w).get<Platform>());
    });

    world.system<const Window>("SyncWindows").kind<phase::Input>().each([](Entity e, const Window& window) {
        VEACH_ZONE("SyncWindows");
        auto& platform = services(e.world()).get<Platform>();
        WindowPlatformState* state = e.try_get_mut<WindowPlatformState>();
        if (state && state->id != 0) {
            if (!(state->applied.title == window.title && state->applied.width == window.width &&
                  state->applied.height == window.height && state->applied.mode == window.mode &&
                  state->applied.vsync == window.vsync)) {
                platform.update_window(state->id, window, state->applied);
                state->applied = window;
            }
            const RelativeMouse* relative = e.try_get<RelativeMouse>();
            const bool wanted = relative != nullptr && relative->enabled;
            if (wanted != state->relative_mouse) {
                platform.set_relative_mouse(state->id, wanted);
                state->relative_mouse = wanted;
            }
            return;
        }
        auto id = platform.create_window(window);
        if (!id) {
            log::error("platform", id.error());
            e.world().quit();
            return;
        }
        log::info("platform", "window '{}' created ({}x{}, {})", window.title, window.width, window.height,
                  platform.name());
        e.set<WindowPlatformState>({.id = *id, .applied = window});
    });
}

void sync_windows(World& world) {
    const Entity system = world.lookup("veach::platform::SyncWindows");
    VEACH_ASSERT(system, "sync_windows() needs PlatformModule to be imported");
    world.system(system).run();
}

NativeWindowHandle native_window_handle(const World& world, Entity window) {
    const WindowPlatformState* state = window.try_get<WindowPlatformState>();
    if (!state) {
        return {};
    }
    return services(world).get<Platform>().native_handle(state->id);
}

}  // namespace veach
