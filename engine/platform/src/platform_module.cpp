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

#include <string>
#include <vector>

namespace veach {

namespace {

constexpr const char* actions_scope = "veach::actions";

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
        if (e.type == PlatformEvent::Type::key_down || e.type == PlatformEvent::Type::key_up ||
            e.type == PlatformEvent::Type::window_resized) {
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
        }
    }

    world.each([&](InputAction& action) {
        action.down = input.is_down(action.primary) || input.is_down(action.secondary);
        action.pressed = input.just_pressed(action.primary) || input.just_pressed(action.secondary);
        action.released = input.just_released(action.primary) || input.just_released(action.secondary);
    });
}

const InputAction* find_action(const World& world, std::string_view name) {
    const Entity e = world.lookup(action_path(name).c_str());
    return e ? e.try_get<InputAction>() : nullptr;
}

}  // namespace

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
        .member(&WindowPlatformState::applied, "applied");

    world.component<Key>();
    world.component<Input>()
        .member(&Input::down, "down")
        .member(&Input::pressed, "pressed")
        .member(&Input::released, "released")
        .add(flecs::Singleton);
    world.component<InputAction>()
        .member(&InputAction::primary, "primary")
        .member(&InputAction::secondary, "secondary")
        .member(&InputAction::down, "down")
        .member(&InputAction::pressed, "pressed")
        .member(&InputAction::released, "released");
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
