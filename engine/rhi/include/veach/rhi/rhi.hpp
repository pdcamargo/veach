#pragma once

// The render hardware interface (ADR 0009). Thin and modern-only: bindless
// resources, hardware ray tracing, no legacy paths. Two real backends are
// planned from day one — Metal (first) and DX12 — plus a null backend that
// needs no GPU, used by headless runs, CI and agents.
//
// v0 is the seam only: device creation, capabilities and surfaces. Resources,
// command recording and pipelines arrive with the render graph (VCH-010).

#include <veach/core/error.hpp>
#include <veach/platform/native_window.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace veach::rhi {

enum class Backend : std::uint8_t { null, metal, dx12 };

struct Capabilities {
    std::string device_name;
    bool hardware_ray_tracing = false;
    bool bindless = false;
    bool mesh_shaders = false;
};

struct DeviceDesc {
    Backend backend = Backend::null;
    bool validation = false;  ///< Enable API validation layers where available.
};

/// A presentable surface created from a NativeWindowHandle.
struct SurfaceId {
    std::uint32_t value = 0;
    [[nodiscard]] constexpr bool is_valid() const { return value != 0; }
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual Backend backend() const = 0;
    [[nodiscard]] virtual const Capabilities& capabilities() const = 0;

    [[nodiscard]] virtual Result<SurfaceId> create_surface(const NativeWindowHandle& window) = 0;
    virtual void destroy_surface(SurfaceId surface) = 0;
};

[[nodiscard]] Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc);
[[nodiscard]] std::string_view to_string(Backend backend);

}  // namespace veach::rhi
