#pragma once

#include <veach/rhi/rhi.hpp>

#include "../slot_map.hpp"

#include <vector>

namespace veach::rhi {

/// No GPU. Every call succeeds and does nothing; resources exist only as
/// bookkeeping (upload/readback buffers get host memory so map() works).
/// The default backend, and the one headless CI and agents use.
class NullDevice final : public Device {
public:
    explicit NullDevice(std::uint32_t frames_in_flight = 2);

    [[nodiscard]] Backend backend() const override { return Backend::null; }
    [[nodiscard]] const Capabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Result<BufferHandle> create_buffer(const BufferDesc& desc,
                                                     std::span<const std::byte> initial) override;
    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc& desc) override;
    [[nodiscard]] Result<SamplerHandle> create_sampler(const SamplerDesc& desc) override;
    [[nodiscard]] Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
    [[nodiscard]] Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc& desc) override;
    [[nodiscard]] Result<AccelHandle> create_blas(const BlasDesc& desc) override;
    [[nodiscard]] Result<AccelHandle> create_tlas(const TlasDesc& desc) override;
    void destroy(BufferHandle buffer) override { buffers_.erase(buffer); }
    void destroy(TextureHandle texture) override { textures_.erase(texture); }
    void destroy(SamplerHandle sampler) override { samplers_.erase(sampler); }
    void destroy(PipelineHandle pipeline) override { pipelines_.erase(pipeline); }
    void destroy(AccelHandle accel) override { accels_.erase(accel); }
    [[nodiscard]] void* map(BufferHandle buffer) override;
    [[nodiscard]] const TextureDesc* texture_desc(TextureHandle texture) const override {
        return textures_.get(texture);
    }

    [[nodiscard]] DescriptorHandle descriptor(BufferHandle buffer, View view) override;
    [[nodiscard]] DescriptorHandle descriptor(TextureHandle texture, View view) override;
    [[nodiscard]] DescriptorHandle descriptor(SamplerHandle sampler) override;
    [[nodiscard]] DescriptorHandle descriptor(AccelHandle accel) override;

    [[nodiscard]] Result<SurfaceId> create_surface(const NativeWindowHandle& window, const SurfaceDesc& desc) override;
    void resize_surface(SurfaceId surface, std::uint32_t width, std::uint32_t height) override;
    [[nodiscard]] Result<TextureHandle> acquire_image(SurfaceId surface) override;
    void destroy_surface(SurfaceId surface) override;

    [[nodiscard]] std::unique_ptr<CommandList> begin_commands(std::string_view debug_name) override;
    SubmissionId submit(std::unique_ptr<CommandList> commands, SurfaceId present) override;
    void wait(SubmissionId) override {}
    void wait_idle() override { tracker_.collect(); }
    FrameInfo begin_frame() override {
        SubmissionId unused;
        const FrameInfo frame = tracker_.begin_frame(unused);
        tracker_.collect();
        return frame;
    }
    void end_frame() override { tracker_.end_frame(); }
    [[nodiscard]] FrameInfo current_frame() const override { return tracker_.current_frame(); }
    [[nodiscard]] std::uint32_t frames_in_flight() const override { return tracker_.frames_in_flight(); }
    [[nodiscard]] double last_frame_gpu_ms() const override { return 0.0; }

private:
    struct Buffer {
        BufferDesc desc;
        std::vector<std::byte> host;  ///< upload/readback only
    };
    struct Surface {
        SurfaceDesc desc;
        TextureHandle image;
    };

    Capabilities caps_;
    SlotMap<Buffer, BufferHandle> buffers_;
    SlotMap<TextureDesc, TextureHandle> textures_;
    SlotMap<SamplerDesc, SamplerHandle> samplers_;
    SlotMap<int, PipelineHandle> pipelines_;
    SlotMap<int, AccelHandle> accels_;
    std::vector<Surface> surfaces_;  ///< SurfaceId is index + 1
    SubmissionTracker tracker_;      ///< submissions "complete" immediately
};

}  // namespace veach::rhi
