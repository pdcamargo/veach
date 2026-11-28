#include "null_device.hpp"

#include "../indirect_validation.hpp"
#include "../pipeline_validation.hpp"
#include "../surface_validation.hpp"
#include "../texture_validation.hpp"

#include <veach/core/assert.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace veach::rhi {

namespace {

class NullCommandList final : public CommandList {
public:
    explicit NullCommandList(const NullDevice& device) : device_(device) {}

    void begin_render_pass(const RenderPassDesc&) override {}
    void end_render_pass() override {}
    void begin_compute_pass(std::string_view) override {}
    void end_compute_pass() override {}
    void set_pipeline(PipelineHandle) override {}
    void set_parameters(const void*, std::size_t size) override {
        VEACH_ASSERT(size <= max_parameter_bytes, "parameter block of {} bytes exceeds {}", size, max_parameter_bytes);
    }
    void draw(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    // Nothing runs, but the arguments are checked as the GPU backends check them.
    void dispatch_indirect(BufferHandle args, std::uint64_t offset) override {
        check_indirect_args(device_.buffer_desc(args), offset, 1, sizeof(DispatchIndirectArgs),
                            sizeof(DispatchIndirectArgs), "dispatch_indirect");
    }
    void draw_indirect(BufferHandle args, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) override {
        check_indirect_args(device_.buffer_desc(args), offset, count, stride, sizeof(DrawIndirectArgs),
                            "draw_indirect");
    }
    void draw_indexed_indirect(BufferHandle indices, BufferHandle args, std::uint64_t offset, std::uint32_t count,
                               std::uint32_t stride) override {
        check_index_buffer(device_.buffer_desc(indices), "draw_indexed_indirect");
        check_indirect_args(device_.buffer_desc(args), offset, count, stride, sizeof(DrawIndexedIndirectArgs),
                            "draw_indexed_indirect");
    }
    void build_blas(AccelHandle) override {}
    void refit_blas(AccelHandle) override {}
    void build_tlas(AccelHandle, std::span<const TlasInstance>) override {}
    void refit_tlas(AccelHandle, std::span<const TlasInstance>) override {}
    void copy_texture_to_buffer(TextureHandle, BufferHandle) override {}
    void copy_buffer(BufferHandle, BufferHandle, std::uint64_t) override {}
    void use(ResourceRef, ResourceState) override {}
    void barrier(std::span<const Barrier>) override {}
    void push_debug_group(std::string_view) override {}
    void pop_debug_group() override {}
    void begin_timing(std::string_view) override {}
    void end_timing() override {}

private:
    const NullDevice& device_;
};

}  // namespace

NullDevice::NullDevice(std::uint32_t frames_in_flight) : tracker_(frames_in_flight) {
    caps_.device_name = "null";
}

Result<BufferHandle> NullDevice::create_buffer(const BufferDesc& desc, std::span<const std::byte> initial) {
    Buffer buffer{desc, {}};
    if (desc.memory != MemoryType::gpu) {
        buffer.host.resize(desc.size);
        if (!initial.empty()) {
            std::memcpy(buffer.host.data(), initial.data(), std::min<std::size_t>(initial.size(), desc.size));
        }
    }
    return buffers_.insert(std::move(buffer));
}

Result<TextureHandle> NullDevice::create_texture(const TextureDesc& desc, std::span<const std::span<const std::byte>>) {
    if (auto valid = validate_texture_desc(desc); !valid) {
        return std::unexpected(valid.error());
    }
    return textures_.insert(desc);
}

Result<SamplerHandle> NullDevice::create_sampler(const SamplerDesc& desc) {
    return samplers_.insert(desc);
}

Result<PipelineHandle> NullDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    if (auto valid = validate_graphics_pipeline_desc(desc); !valid) {
        return std::unexpected(valid.error());
    }
    return pipelines_.insert(0);
}

Result<PipelineHandle> NullDevice::create_compute_pipeline(const ComputePipelineDesc&) {
    return pipelines_.insert(0);
}

Result<AccelHandle> NullDevice::create_blas(const BlasDesc&) {
    return accels_.insert(0);
}

Result<AccelHandle> NullDevice::create_tlas(const TlasDesc&) {
    return accels_.insert(0);
}

void* NullDevice::map(BufferHandle buffer) {
    Buffer* b = buffers_.get(buffer);
    return b && !b->host.empty() ? b->host.data() : nullptr;
}

DescriptorHandle NullDevice::descriptor(BufferHandle buffer, View) {
    return {buffer.index};
}
DescriptorHandle NullDevice::descriptor(TextureHandle texture, View) {
    return {texture.index};
}
DescriptorHandle NullDevice::descriptor(SamplerHandle sampler) {
    return {sampler.index};
}
DescriptorHandle NullDevice::descriptor(AccelHandle accel) {
    return {accel.index};
}

Result<SurfaceId> NullDevice::create_surface(const NativeWindowHandle& window, const SurfaceDesc& desc) {
    if (auto valid = validate_surface_handle(window); !valid) {
        return std::unexpected(valid.error());
    }
    auto image = create_texture({desc.width, desc.height, surface_format, TextureUsage::color_target, "surface"}, {});
    surfaces_.push_back({desc, *image});
    return SurfaceId{static_cast<std::uint32_t>(surfaces_.size())};
}

void NullDevice::resize_surface(SurfaceId surface, std::uint32_t width, std::uint32_t height) {
    if (surface.is_valid() && surface.value <= surfaces_.size()) {
        Surface& s = surfaces_[surface.value - 1];
        s.desc.width = width;
        s.desc.height = height;
        if (TextureDesc* image = textures_.get(s.image)) {
            image->width = width;
            image->height = height;
        }
    }
}

Result<TextureHandle> NullDevice::acquire_image(SurfaceId surface) {
    if (!surface.is_valid() || surface.value > surfaces_.size()) {
        return make_error(ErrorCode::rhi_invalid_surface, "unknown surface");
    }
    return surfaces_[surface.value - 1].image;
}

void NullDevice::destroy_surface(SurfaceId) {}

std::unique_ptr<CommandList> NullDevice::begin_commands(std::string_view) {
    return std::make_unique<NullCommandList>(*this);
}

SubmissionId NullDevice::submit(std::unique_ptr<CommandList>, SurfaceId) {
    const SubmissionId id = tracker_.next_submission();
    tracker_.mark_completed(id);
    tracker_.collect();
    return id;
}

}  // namespace veach::rhi
