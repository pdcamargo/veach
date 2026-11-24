#pragma once

#include <veach/rhi/rhi.hpp>

namespace veach::rhi {

/// No GPU. Accepts surfaces (validating the native handle) and does nothing.
/// The default backend until rendering exists, and always the one for --headless.
class NullDevice final : public Device {
public:
    NullDevice();

    [[nodiscard]] Backend backend() const override { return Backend::null; }
    [[nodiscard]] const Capabilities& capabilities() const override { return caps_; }
    [[nodiscard]] Result<SurfaceId> create_surface(const NativeWindowHandle& window) override;
    void destroy_surface(SurfaceId surface) override;

private:
    Capabilities caps_;
    std::uint32_t next_surface_ = 1;
};

}  // namespace veach::rhi
