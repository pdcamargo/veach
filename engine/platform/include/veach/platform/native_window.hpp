#pragma once

// The only thing the RHI ever learns about a window. No SDL types: the platform
// layer fills this in, the RHI creates its surface/swapchain from it.

#include <cstdint>

namespace veach {

struct NativeWindowHandle {
    enum class Kind : std::uint8_t {
        none,   ///< Headless: no OS window exists.
        metal,  ///< macOS: `metal_layer` is a CAMetalLayer*.
        win32,  ///< Windows: `hwnd` is an HWND, `hinstance` an HINSTANCE.
    };

    Kind kind = Kind::none;
    void* metal_layer = nullptr;
    void* hwnd = nullptr;
    void* hinstance = nullptr;
    std::uint32_t width = 0;  ///< Drawable size in pixels.
    std::uint32_t height = 0;

    [[nodiscard]] bool is_valid() const {
        switch (kind) {
            case Kind::none: return false;
            case Kind::metal: return metal_layer != nullptr;
            case Kind::win32: return hwnd != nullptr;
        }
        return false;
    }
};

}  // namespace veach
