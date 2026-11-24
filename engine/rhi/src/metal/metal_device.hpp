#pragma once

#include <veach/rhi/rhi.hpp>

namespace veach::rhi {

/// Metal backend (stub). Creates the system default MTLDevice, reports its
/// capabilities and binds it to CAMetalLayer surfaces. No rendering yet (VCH-010).
[[nodiscard]] Result<std::unique_ptr<Device>> create_metal_device(const DeviceDesc& desc);

}  // namespace veach::rhi
