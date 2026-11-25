#include <veach/rhi/rhi.hpp>

#include <format>

#include "null/null_device.hpp"
#if defined(__APPLE__)
#include "metal/metal_device.hpp"
#endif
#include "dx12/dx12_device.hpp"

namespace veach::rhi {

std::string_view to_string(Backend backend) {
    switch (backend) {
        case Backend::null: return "null";
        case Backend::metal: return "metal";
        case Backend::dx12: return "dx12";
    }
    return "?";
}

Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc) {
    switch (desc.backend) {
        case Backend::null: return std::unique_ptr<Device>(std::make_unique<NullDevice>());
        case Backend::metal:
#if defined(__APPLE__)
            return create_metal_device(desc);
#else
            break;
#endif
        case Backend::dx12:
#if defined(_WIN32)
            return create_dx12_device(desc);
#else
            static_cast<void>(&create_dx12_device);
            break;
#endif
    }
    return make_error(ErrorCode::rhi_backend_unavailable,
                      std::format("the '{}' RHI backend is not available on this platform", to_string(desc.backend)),
                      "Use \"rhi\": \"metal\" on macOS, \"dx12\" on Windows, or \"null\" anywhere.");
}

}  // namespace veach::rhi
