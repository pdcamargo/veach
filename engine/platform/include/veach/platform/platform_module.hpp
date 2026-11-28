#pragma once

// ECS side of the platform layer. Registers Window, Input and InputAction, and
// two Input-phase systems: PollPlatform (OS events -> Input, actions, window
// close/resize) and SyncWindows (Window component -> OS window).
// Requires a Platform in the world's Services.

#include <veach/ecs/flecs.hpp>
#include <veach/platform/native_window.hpp>

namespace veach {

struct PlatformModule {
    explicit PlatformModule(World& world);
};

/// Creates/updates/destroys OS windows so they match Window components now,
/// without waiting for the Input phase. The engine calls this during init so
/// the primary window exists before the RHI is created.
void sync_windows(World& world);

/// Relative axes (mouse motion and wheel) hold the motion since the last fixed step. The engine calls this after
/// a frame's first fixed step, so later steps and phases read 0 and a frame without steps keeps the motion for
/// the next one: motion is never lost or counted twice, whatever the frame rate.
void consume_relative_axes(World& world);

/// The native handle of a window entity (Kind::none if it has no OS window).
[[nodiscard]] NativeWindowHandle native_window_handle(const World& world, Entity window);

}  // namespace veach
