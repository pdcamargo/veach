#pragma once

// Windows are entities (ADR 0008). Set or change the Window component and the
// platform layer makes the OS window match during the Input phase; tools and
// MCP control windows with ordinary set_component calls.

#include <cstdint>
#include <string>

namespace veach {

enum class WindowMode : std::uint8_t {
    windowed,
    fullscreen,  ///< Borderless fullscreen on the window's display.
};

struct Window {
    std::string title = "Veach";
    std::uint32_t width = 1280;  ///< Logical size (points), not pixels.
    std::uint32_t height = 720;
    WindowMode mode = WindowMode::windowed;
    bool vsync = true;  ///< Read by the renderer when it creates the swapchain.
};

/// Tag for the window whose closing ends the app. The engine adds it to the
/// window it creates from the project config.
struct PrimaryWindow {};

/// Platform bookkeeping, owned by the platform module. `applied` is what the OS
/// window currently reflects; diffing it against Window drives updates.
struct WindowPlatformState {
    std::uint32_t id = 0;
    Window applied;
};

}  // namespace veach
