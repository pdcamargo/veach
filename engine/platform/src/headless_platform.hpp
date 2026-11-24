#pragma once

#include <veach/platform/platform.hpp>

namespace veach {

/// No SDL, no OS windows, no events. Windows still get ids so the ECS side
/// behaves identically; their native handles are Kind::none.
class HeadlessPlatform final : public Platform {
public:
    [[nodiscard]] PlatformBackend backend() const override { return PlatformBackend::headless; }
    [[nodiscard]] std::string_view name() const override { return "headless"; }

    [[nodiscard]] Result<std::uint32_t> create_window(const Window& desc) override;
    void destroy_window(std::uint32_t id) override;
    void update_window(std::uint32_t id, const Window& desc, const Window& previous) override;
    [[nodiscard]] NativeWindowHandle native_handle(std::uint32_t id) const override;
    void poll_events(std::vector<PlatformEvent>& out) override;

private:
    std::uint32_t next_id_ = 1;
};

}  // namespace veach
