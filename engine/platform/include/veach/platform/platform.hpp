#pragma once

// The platform backend interface (ADR 0012). Two backends exist:
//   * sdl      — SDL3 windows and input. SDL never appears outside engine/platform.
//   * headless — no SDL video, no OS windows, no events. Used by CI and agents.
// Engine and gameplay code talk to windows through the Window component; only
// the platform module and the RHI handshake use this interface directly.

#include <veach/core/error.hpp>
#include <veach/platform/input.hpp>
#include <veach/platform/native_window.hpp>
#include <veach/platform/window.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace veach {

enum class PlatformBackend : std::uint8_t { headless, sdl };

struct PlatformEvent {
    enum class Type : std::uint8_t { quit_requested, window_close_requested, window_resized, key_down, key_up };
    Type type = Type::quit_requested;
    std::uint32_t window = 0;
    Key key = Key::unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class Platform {
public:
    virtual ~Platform() = default;

    [[nodiscard]] virtual PlatformBackend backend() const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Creates a window matching `desc`. Returns a nonzero platform window id.
    [[nodiscard]] virtual Result<std::uint32_t> create_window(const Window& desc) = 0;
    virtual void destroy_window(std::uint32_t id) = 0;
    /// Makes the OS window match `desc` (title, size, mode). Only changed fields
    /// need to be applied; `previous` is what was applied last time.
    virtual void update_window(std::uint32_t id, const Window& desc, const Window& previous) = 0;
    [[nodiscard]] virtual NativeWindowHandle native_handle(std::uint32_t id) const = 0;

    /// Appends all pending OS events to `out`.
    virtual void poll_events(std::vector<PlatformEvent>& out) = 0;

    /// Queues an event as if the OS had sent it (tests, MCP `send_input`). Delivered with the next poll.
    void inject(const PlatformEvent& event) { injected_.push_back(event); }
    /// Moves the injected events to `out`, after the OS ones.
    void take_injected(std::vector<PlatformEvent>& out) {
        out.insert(out.end(), injected_.begin(), injected_.end());
        injected_.clear();
    }

    /// Replaying (docs/specs/record-replay/spec.md): the OS's input and resize events are dropped, so only the
    /// recorded ones (injected) drive the run; quit and close requests still count.
    void set_replaying(bool replaying) { replaying_ = replaying; }
    [[nodiscard]] bool replaying() const { return replaying_; }
    /// The input and resize events applied in the last Input phase, OS and injected: what a recording keeps.
    [[nodiscard]] const std::vector<PlatformEvent>& applied_events() const { return applied_; }
    void set_applied_events(std::vector<PlatformEvent> events) { applied_ = std::move(events); }

private:
    std::vector<PlatformEvent> injected_;
    std::vector<PlatformEvent> applied_;
    bool replaying_ = false;
};

[[nodiscard]] Result<std::unique_ptr<Platform>> create_platform(PlatformBackend backend);
[[nodiscard]] std::string_view to_string(PlatformBackend backend);

}  // namespace veach
