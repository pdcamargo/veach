#include "dx12_device.hpp"

#include <format>
#include <type_traits>

namespace veach::rhi {

namespace {

constexpr const char* stub_hint = "The DX12 backend is a stub until VCH-012; use \"rhi\": \"null\" on Windows for now.";

std::unexpected<Error> not_implemented(const char* what) {
    return make_error(ErrorCode::rhi_not_implemented, std::format("DX12: {} is not implemented", what), stub_hint);
}

/// The complete Device interface, implemented as stubs. Compiling this on every
/// Windows build keeps the RHI honest about DX12 (ADR 0024/0025 list what the
/// real implementation must handle).
// TODO(VCH-012): implement on ID3D12Device10 + enhanced barriers + SM 6.6 descriptor heaps.
class Dx12Device final : public Device {
public:
    [[nodiscard]] Backend backend() const override { return Backend::dx12; }
    [[nodiscard]] const Capabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Result<BufferHandle> create_buffer(const BufferDesc&, std::span<const std::byte>) override {
        return not_implemented("create_buffer");
    }
    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc&) override {
        return not_implemented("create_texture");
    }
    [[nodiscard]] Result<SamplerHandle> create_sampler(const SamplerDesc&) override {
        return not_implemented("create_sampler");
    }
    [[nodiscard]] Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc& desc) override {
        // DXIL comes from ShaderLibrary::find(entry)->dxil (compiled for sm_6_6 by cmake/VeachShaders.cmake).
        static_cast<void>(desc);
        return not_implemented("create_graphics_pipeline");
    }
    [[nodiscard]] Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc&) override {
        return not_implemented("create_compute_pipeline");
    }
    [[nodiscard]] Result<AccelHandle> create_blas(const BlasDesc&) override { return not_implemented("create_blas"); }
    [[nodiscard]] Result<AccelHandle> create_tlas(const TlasDesc&) override { return not_implemented("create_tlas"); }
    void destroy(BufferHandle) override {}
    void destroy(TextureHandle) override {}
    void destroy(SamplerHandle) override {}
    void destroy(PipelineHandle) override {}
    void destroy(AccelHandle) override {}
    [[nodiscard]] void* map(BufferHandle) override { return nullptr; }
    [[nodiscard]] const TextureDesc* texture_desc(TextureHandle) const override { return nullptr; }
    [[nodiscard]] DescriptorHandle descriptor(BufferHandle, View) override { return {}; }
    [[nodiscard]] DescriptorHandle descriptor(TextureHandle, View) override { return {}; }
    [[nodiscard]] DescriptorHandle descriptor(SamplerHandle) override { return {}; }
    [[nodiscard]] DescriptorHandle descriptor(AccelHandle) override { return {}; }
    [[nodiscard]] Result<SurfaceId> create_surface(const NativeWindowHandle&, const SurfaceDesc&) override {
        return not_implemented("create_surface");
    }
    void resize_surface(SurfaceId, std::uint32_t, std::uint32_t) override {}
    [[nodiscard]] Result<TextureHandle> acquire_image(SurfaceId) override { return not_implemented("acquire_image"); }
    void destroy_surface(SurfaceId) override {}
    [[nodiscard]] std::unique_ptr<CommandList> begin_commands(std::string_view) override { return nullptr; }
    SubmissionId submit(std::unique_ptr<CommandList>, SurfaceId) override { return {}; }
    void wait(SubmissionId) override {}
    void wait_idle() override {}

private:
    Capabilities caps_{.device_name = "dx12 (stub)"};
};

static_assert(!std::is_abstract_v<Dx12Device>, "the DX12 stub must implement the whole rhi::Device interface");

}  // namespace

Result<std::unique_ptr<Device>> create_dx12_device(const DeviceDesc& /*desc*/) {
    return not_implemented("device creation");
}

}  // namespace veach::rhi
