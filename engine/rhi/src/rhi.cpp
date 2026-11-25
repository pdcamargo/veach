#include <veach/rhi/rhi.hpp>

#include <veach/core/assert.hpp>
#include <veach/rhi/command_list.hpp>

#include <format>

#include "null/null_device.hpp"
#if defined(__APPLE__)
#include "metal/metal_device.hpp"
#endif
#if defined(_WIN32)
#include "dx12/dx12_device.hpp"
#endif

namespace veach::rhi {

std::string_view to_string(Backend backend) {
    switch (backend) {
        case Backend::null: return "null";
        case Backend::metal: return "metal";
        case Backend::dx12: return "dx12";
    }
    return "?";
}

// Vendor temporal upscalers (ADR 0043): only backends that have one override these.
Result<TemporalScalerId> Device::create_temporal_scaler(const TemporalScalerDesc&) {
    return make_error(ErrorCode::rhi_backend_unavailable,
                      std::format("the {} backend has no vendor temporal upscaler", to_string(backend())),
                      "Use Veach's own upscaler (Upscaling.mode = \"temporal\").");
}

void Device::destroy_temporal_scaler(TemporalScalerId) {}

void CommandList::temporal_scale(TemporalScalerId, const TemporalScale&) {
    VEACH_ASSERT(false, "temporal_scale on a backend without a vendor temporal upscaler");
}

Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc) {
    switch (desc.backend) {
        case Backend::null: return std::unique_ptr<Device>(std::make_unique<NullDevice>(desc.frames_in_flight));
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
            break;
#endif
    }
    return make_error(ErrorCode::rhi_backend_unavailable,
                      std::format("the '{}' RHI backend is not available on this platform", to_string(desc.backend)),
                      "Use \"rhi\": \"metal\" on macOS, \"dx12\" on Windows, or \"null\" anywhere.");
}

}  // namespace veach::rhi
