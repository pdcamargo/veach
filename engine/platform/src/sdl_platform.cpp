// The only translation unit that includes SDL. Keep it that way (ADR 0008).

#include "sdl_platform.hpp"

#include <veach/core/log.hpp>

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED  // we own main(); only SDL_SetMainReady is needed
#include <SDL3/SDL_main.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif

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

}  // namespace

struct SdlPlatform::Impl {
    struct WindowEntry {
        SDL_Window* window = nullptr;
#if defined(__APPLE__)
        SDL_MetalView metal_view = nullptr;
#endif
    };
    std::unordered_map<std::uint32_t, WindowEntry> windows;
};

Result<std::unique_ptr<SdlPlatform>> SdlPlatform::create() {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return make_error(ErrorCode::platform_init_failed, std::format("SDL_Init failed: {}", SDL_GetError()),
                          "Run with --headless on machines without a display (CI, SSH, containers).");
    }
    auto platform = std::unique_ptr<SdlPlatform>(new SdlPlatform());
    platform->impl_ = std::make_unique<Impl>();
    log::debug("platform", "SDL {}.{}.{} initialized (video driver: {})", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
               SDL_MICRO_VERSION, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    return platform;
}

SdlPlatform::~SdlPlatform() {
    if (impl_) {
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
            default: break;
        }
    }
}

}  // namespace veach
