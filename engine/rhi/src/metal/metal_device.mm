#include "metal_device.hpp"

#include "../surface_validation.hpp"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <unordered_map>

namespace veach::rhi {

namespace {

class MetalDevice final : public Device {
public:
    explicit MetalDevice(id<MTLDevice> device) : device_(device) {
        caps_.device_name = device.name.UTF8String;
        caps_.hardware_ray_tracing = device.supportsRaytracing;
        caps_.bindless = [device supportsFamily:MTLGPUFamilyApple6] || [device supportsFamily:MTLGPUFamilyMac2];
        caps_.mesh_shaders = [device supportsFamily:MTLGPUFamilyApple7] || [device supportsFamily:MTLGPUFamilyMac2];
    }

    [[nodiscard]] Backend backend() const override { return Backend::metal; }
    [[nodiscard]] const Capabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Result<SurfaceId> create_surface(const NativeWindowHandle& window) override {
        if (auto valid = validate_surface_handle(window); !valid) {
            return std::unexpected(valid.error());
        }
        if (window.kind != NativeWindowHandle::Kind::metal) {
            return make_error(ErrorCode::rhi_invalid_surface, "the Metal backend needs a CAMetalLayer surface");
        }
        CAMetalLayer* layer = (__bridge CAMetalLayer*)window.metal_layer;
        layer.device = device_;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        const SurfaceId id{next_surface_++};
        layers_[id.value] = layer;
        return id;
    }

    void destroy_surface(SurfaceId surface) override { layers_.erase(surface.value); }

private:
    id<MTLDevice> device_;
    Capabilities caps_;
    std::uint32_t next_surface_ = 1;
    std::unordered_map<std::uint32_t, CAMetalLayer*> layers_;
};

}  // namespace

Result<std::unique_ptr<Device>> create_metal_device(const DeviceDesc& /*desc*/) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        return make_error(ErrorCode::rhi_backend_unavailable, "no Metal device is available",
                          "Use \"rhi\": \"null\" (the default) on machines without a GPU.");
    }
    return std::unique_ptr<Device>(std::make_unique<MetalDevice>(device));
}

}  // namespace veach::rhi
