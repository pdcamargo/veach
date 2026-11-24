#pragma once

// SDL types stay in sdl_platform.cpp; this header is SDL-free on purpose.

#include <veach/platform/platform.hpp>

#include <memory>

namespace veach {

class SdlPlatform final : public Platform {
public:
    [[nodiscard]] static Result<std::unique_ptr<SdlPlatform>> create();
    ~SdlPlatform() override;
    SdlPlatform(const SdlPlatform&) = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;
    SdlPlatform(SdlPlatform&&) = delete;
    SdlPlatform& operator=(SdlPlatform&&) = delete;

    [[nodiscard]] PlatformBackend backend() const override { return PlatformBackend::sdl; }
    [[nodiscard]] std::string_view name() const override { return "sdl3"; }

    [[nodiscard]] Result<std::uint32_t> create_window(const Window& desc) override;
    void destroy_window(std::uint32_t id) override;
    void update_window(std::uint32_t id, const Window& desc, const Window& previous) override;
    [[nodiscard]] NativeWindowHandle native_handle(std::uint32_t id) const override;
    void poll_events(std::vector<PlatformEvent>& out) override;

private:
    SdlPlatform() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace veach
