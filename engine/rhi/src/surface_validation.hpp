#pragma once

#include <veach/core/log.hpp>
#include <veach/rhi/rhi.hpp>

#include <format>

namespace veach::rhi {

/// Every backend runs this before creating a surface, and logs what it got.
inline Result<> validate_surface_handle(const NativeWindowHandle& window) {
    if (!window.is_valid()) {
        return make_error(ErrorCode::rhi_invalid_surface, "the native window handle is empty or incomplete",
                          "Surfaces need a window created by the SDL platform backend; headless runs have none.");
    }
    switch (window.kind) {
        case NativeWindowHandle::Kind::metal:
            log::debug("rhi", "native window handle is valid: CAMetalLayer={} ({}x{} px)", window.metal_layer,
                       window.width, window.height);
            break;
        case NativeWindowHandle::Kind::win32:
            log::debug("rhi", "native window handle is valid: HWND={} ({}x{} px)", window.hwnd, window.width,
                       window.height);
            break;
        case NativeWindowHandle::Kind::none: break;
    }
    return {};
}

}  // namespace veach::rhi
