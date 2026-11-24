#include "dx12_device.hpp"

namespace veach::rhi {

Result<std::unique_ptr<Device>> create_dx12_device(const DeviceDesc& /*desc*/) {
    // TODO(VCH-012): create an ID3D12Device (DXR 1.1 required) and DXGI swapchains.
    return make_error(ErrorCode::rhi_not_implemented, "the DX12 backend is a stub",
                      "Use \"rhi\": \"null\" until VCH-012 lands.");
}

}  // namespace veach::rhi
