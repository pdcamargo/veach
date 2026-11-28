// The only translation unit that includes SDL. Keep it that way (ADR 0012).

#include "sdl_platform.hpp"

#include <veach/core/log.hpp>

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED  // we own main(); only SDL_SetMainReady is needed
#include <SDL3/SDL_main.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif

#include <algorithm>
#include <format>
#include <unordered_map>

namespace veach {

namespace {

Key translate_scancode(SDL_Scancode code) {
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<int>(Key::a) + (code - SDL_SCANCODE_A));
    }
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<int>(Key::digit1) + (code - SDL_SCANCODE_1));
    }
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12) {
        return static_cast<Key>(static_cast<int>(Key::f1) + (code - SDL_SCANCODE_F1));
    }
    switch (code) {
        case SDL_SCANCODE_0: return Key::digit0;
        case SDL_SCANCODE_SPACE: return Key::space;
        case SDL_SCANCODE_RETURN: return Key::enter;
        case SDL_SCANCODE_ESCAPE: return Key::escape;
        case SDL_SCANCODE_TAB: return Key::tab;
        case SDL_SCANCODE_BACKSPACE: return Key::backspace;
        case SDL_SCANCODE_LEFT: return Key::left;
        case SDL_SCANCODE_RIGHT: return Key::right;
        case SDL_SCANCODE_UP: return Key::up;
        case SDL_SCANCODE_DOWN: return Key::down;
        case SDL_SCANCODE_LSHIFT: return Key::left_shift;
        case SDL_SCANCODE_RSHIFT: return Key::right_shift;
        case SDL_SCANCODE_LCTRL: return Key::left_ctrl;
        case SDL_SCANCODE_RCTRL: return Key::right_ctrl;
        case SDL_SCANCODE_LALT: return Key::left_alt;
        case SDL_SCANCODE_RALT: return Key::right_alt;
        default: return Key::unknown;
    }
}

Key translate_gamepad_button(Uint8 button) {
    switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return Key::gamepad_south;
        case SDL_GAMEPAD_BUTTON_EAST: return Key::gamepad_east;
        case SDL_GAMEPAD_BUTTON_WEST: return Key::gamepad_west;
        case SDL_GAMEPAD_BUTTON_NORTH: return Key::gamepad_north;
        case SDL_GAMEPAD_BUTTON_BACK: return Key::gamepad_back;
        case SDL_GAMEPAD_BUTTON_GUIDE: return Key::gamepad_guide;
        case SDL_GAMEPAD_BUTTON_START: return Key::gamepad_start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return Key::gamepad_left_stick;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return Key::gamepad_right_stick;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return Key::gamepad_left_shoulder;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return Key::gamepad_right_shoulder;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return Key::gamepad_dpad_up;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Key::gamepad_dpad_down;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Key::gamepad_dpad_left;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Key::gamepad_dpad_right;
        default: return Key::unknown;
    }
}

/// A gamepad axis event as an Axis and its value in -1..1 (triggers 0..1), +Y up.
PlatformEvent translate_gamepad_axis(Uint8 axis, Sint16 raw) {
    double value = std::clamp(static_cast<double>(raw) / 32767.0, -1.0, 1.0);
    Axis out = Axis::none;
    switch (static_cast<SDL_GamepadAxis>(axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX: out = Axis::gamepad_left_x; break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            out = Axis::gamepad_left_y;
            value = -value;
            break;  // SDL: +Y is down
        case SDL_GAMEPAD_AXIS_RIGHTX: out = Axis::gamepad_right_x; break;
        case SDL_GAMEPAD_AXIS_RIGHTY:
            out = Axis::gamepad_right_y;
            value = -value;
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: out = Axis::gamepad_left_trigger; break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: out = Axis::gamepad_right_trigger; break;
        default: break;
    }
    return {.type = PlatformEvent::Type::axis_moved, .axis = out, .value = quantize_axis(value)};
}

}  // namespace

struct SdlPlatform::Impl {
    struct WindowEntry {
        SDL_Window* window = nullptr;
#if defined(__APPLE__)
        SDL_MetalView metal_view = nullptr;
#endif
    };
    std::unordered_map<std::uint32_t, WindowEntry> windows;
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads;
};

Result<std::unique_ptr<SdlPlatform>> SdlPlatform::create() {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return make_error(ErrorCode::platform_init_failed, std::format("SDL_Init failed: {}", SDL_GetError()),
                          "Run with --headless on machines without a display (CI, SSH, containers).");
    }
    auto platform = std::unique_ptr<SdlPlatform>(new SdlPlatform());
    platform->impl_ = std::make_unique<Impl>();
    // Gamepads are optional: without them keyboard and mouse still work. Connected pads arrive as
    // SDL_EVENT_GAMEPAD_ADDED with the first poll.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        log::warn("platform", "no gamepad support: SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed: {}", SDL_GetError());
    }
    log::debug("platform", "SDL {}.{}.{} initialized (video driver: {})", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    return platform;
}

SdlPlatform::~SdlPlatform() {
    if (impl_) {
        for (const auto& [id, gamepad] : impl_->gamepads) {
            SDL_CloseGamepad(gamepad);
        }
        while (!impl_->windows.empty()) {
            destroy_window(impl_->windows.begin()->first);
        }
    }
    SDL_Quit();
}

Result<std::uint32_t> SdlPlatform::create_window(const Window& desc) {
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
    if (desc.mode == WindowMode::fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    SDL_Window* window =
        SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width), static_cast<int>(desc.height), flags);
    if (!window) {
        return make_error(ErrorCode::platform_window_failed, std::format("SDL_CreateWindow failed: {}", SDL_GetError()),
                          "Check the window size in veach.project.json, or run with --headless.");
    }
    Impl::WindowEntry entry{window};
#if defined(__APPLE__)
    entry.metal_view = SDL_Metal_CreateView(window);
#endif
    const std::uint32_t id = SDL_GetWindowID(window);
    impl_->windows.emplace(id, entry);
    return id;
}

void SdlPlatform::destroy_window(std::uint32_t id) {
    const auto it = impl_->windows.find(id);
    if (it == impl_->windows.end()) {
        return;
    }
#if defined(__APPLE__)
    if (it->second.metal_view) {
        SDL_Metal_DestroyView(it->second.metal_view);
    }
#endif
    SDL_DestroyWindow(it->second.window);
    impl_->windows.erase(it);
}

void SdlPlatform::update_window(std::uint32_t id, const Window& desc, const Window& previous) {
    const auto it = impl_->windows.find(id);
    if (it == impl_->windows.end()) {
        return;
    }
    SDL_Window* window = it->second.window;
    if (desc.title != previous.title) {
        SDL_SetWindowTitle(window, desc.title.c_str());
    }
    if (desc.width != previous.width || desc.height != previous.height) {
        SDL_SetWindowSize(window, static_cast<int>(desc.width), static_cast<int>(desc.height));
    }
    if (desc.mode != previous.mode) {
        SDL_SetWindowFullscreen(window, desc.mode == WindowMode::fullscreen);
    }
}

void SdlPlatform::set_relative_mouse(std::uint32_t id, bool enabled) {
    const auto it = impl_->windows.find(id);
    if (it != impl_->windows.end() && !SDL_SetWindowRelativeMouseMode(it->second.window, enabled)) {
        log::warn("platform", "relative mouse mode {} failed: {}", enabled ? "on" : "off", SDL_GetError());
    }
}

NativeWindowHandle SdlPlatform::native_handle(std::uint32_t id) const {
    NativeWindowHandle handle;
    const auto it = impl_->windows.find(id);
    if (it == impl_->windows.end()) {
        return handle;
    }
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(it->second.window, &w, &h);
    handle.width = static_cast<std::uint32_t>(w);
    handle.height = static_cast<std::uint32_t>(h);
#if defined(__APPLE__)
    handle.kind = NativeWindowHandle::Kind::metal;
    handle.metal_layer = it->second.metal_view ? SDL_Metal_GetLayer(it->second.metal_view) : nullptr;
#elif defined(_WIN32)
    const SDL_PropertiesID props = SDL_GetWindowProperties(it->second.window);
    handle.kind = NativeWindowHandle::Kind::win32;
    handle.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    handle.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
#endif
    return handle;
}

void SdlPlatform::poll_events(std::vector<PlatformEvent>& out) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT: out.push_back({.type = PlatformEvent::Type::quit_requested}); break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                out.push_back({.type = PlatformEvent::Type::window_close_requested, .window = event.window.windowID});
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                out.push_back({.type = PlatformEvent::Type::window_resized,
                               .window = event.window.windowID,
                               .width = static_cast<std::uint32_t>(event.window.data1),
                               .height = static_cast<std::uint32_t>(event.window.data2)});
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (!event.key.repeat) {
                    out.push_back({.type = event.type == SDL_EVENT_KEY_DOWN ? PlatformEvent::Type::key_down
                                                                            : PlatformEvent::Type::key_up,
                                   .window = event.key.windowID,
                                   .key = translate_scancode(event.key.scancode)});
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const Key button = event.button.button == SDL_BUTTON_LEFT     ? Key::mouse_left
                                   : event.button.button == SDL_BUTTON_RIGHT  ? Key::mouse_right
                                   : event.button.button == SDL_BUTTON_MIDDLE ? Key::mouse_middle
                                                                              : Key::unknown;
                out.push_back({.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PlatformEvent::Type::key_down
                                                                                 : PlatformEvent::Type::key_up,
                               .window = event.button.windowID,
                               .key = button});
                break;
            }
            // Mouse motion in window points, +Y up; quantized here so a recording holds exactly what was applied.
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.xrel != 0.0f) {
                    out.push_back({.type = PlatformEvent::Type::axis_moved,
                                   .window = event.motion.windowID,
                                   .axis = Axis::mouse_x,
                                   .value = quantize_axis(event.motion.xrel)});
                }
                if (event.motion.yrel != 0.0f) {
                    out.push_back({.type = PlatformEvent::Type::axis_moved,
                                   .window = event.motion.windowID,
                                   .axis = Axis::mouse_y,
                                   .value = quantize_axis(-event.motion.yrel)});
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: {
                const float sign = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
                if (event.wheel.x != 0.0f) {
                    out.push_back({.type = PlatformEvent::Type::axis_moved,
                                   .window = event.wheel.windowID,
                                   .axis = Axis::mouse_wheel_x,
                                   .value = quantize_axis(sign * event.wheel.x)});
                }
                if (event.wheel.y != 0.0f) {
                    out.push_back({.type = PlatformEvent::Type::axis_moved,
                                   .window = event.wheel.windowID,
                                   .axis = Axis::mouse_wheel_y,
                                   .value = quantize_axis(sign * event.wheel.y)});
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!impl_->gamepads.contains(event.gdevice.which)) {
                    if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which)) {
                        impl_->gamepads.emplace(event.gdevice.which, pad);
                        log::debug("platform", "gamepad '{}' opened",
                                   SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?");
                        out.push_back({.type = PlatformEvent::Type::gamepad_added});
                    } else {
                        log::warn("platform", "a gamepad was connected but cannot be opened: {}", SDL_GetError());
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (const auto it = impl_->gamepads.find(event.gdevice.which); it != impl_->gamepads.end()) {
                    SDL_CloseGamepad(it->second);
                    impl_->gamepads.erase(it);
                    out.push_back({.type = PlatformEvent::Type::gamepad_removed});
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                PlatformEvent axis = translate_gamepad_axis(event.gaxis.axis, event.gaxis.value);
                if (axis.axis != Axis::none) {
                    out.push_back(axis);
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                out.push_back({.type = event.gbutton.down ? PlatformEvent::Type::key_down : PlatformEvent::Type::key_up,
                               .key = translate_gamepad_button(event.gbutton.button)});
                break;
            default: break;
        }
    }
}

}  // namespace veach
