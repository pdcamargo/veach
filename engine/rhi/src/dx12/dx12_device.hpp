#pragma once

#include <veach/rhi/rhi.hpp>

namespace veach::rhi {

/// DX12 backend (VCH-012). Windows only; picks a hardware adapter that meets
/// the feature floor, else WARP (ADR 0031).
[[nodiscard]] Result<std::unique_ptr<Device>> create_dx12_device(const DeviceDesc& desc);

}  // namespace veach::rhi
