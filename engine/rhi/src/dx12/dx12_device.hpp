#pragma once

#include <veach/rhi/rhi.hpp>

namespace veach::rhi {

/// DX12 backend (stub). Exists so the RHI is shaped for two backends from day
/// one; device creation lands with VCH-012.
[[nodiscard]] Result<std::unique_ptr<Device>> create_dx12_device(const DeviceDesc& desc);

}  // namespace veach::rhi
