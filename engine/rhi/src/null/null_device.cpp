#include "null_device.hpp"

#include "../surface_validation.hpp"

namespace veach::rhi {

NullDevice::NullDevice() {
    caps_.device_name = "null";
}

Result<SurfaceId> NullDevice::create_surface(const NativeWindowHandle& window) {
    if (auto valid = validate_surface_handle(window); !valid) {
        return std::unexpected(valid.error());
    }
    return SurfaceId{next_surface_++};
}

void NullDevice::destroy_surface(SurfaceId /*surface*/) {}

}  // namespace veach::rhi
