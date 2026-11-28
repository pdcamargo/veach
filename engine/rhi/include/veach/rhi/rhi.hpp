#pragma once

// The render hardware interface (ADR 0004). Thin and modern-only: bindless
// resources (ADR 0027), explicit barriers derived by the render graph
// (ADR 0029), optional hardware ray tracing (ADR 0003), no legacy paths. Backends:
//   null  — no GPU; every call succeeds and does nothing (headless, CI, agents)
//   metal — macOS, Metal 3
//   dx12  — Windows, D3D12 via the Agility SDK; WARP in hosted CI (ADR 0033)

#include <veach/core/error.hpp>
#include <veach/platform/native_window.hpp>
#include <veach/rhi/command_list.hpp>
#include <veach/rhi/submission_tracker.hpp>
#include <veach/rhi/types.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach::rhi {

enum class Backend : std::uint8_t { null, metal, dx12 };

struct Capabilities {
    std::string device_name;
    bool gpu = false;  ///< false for the null backend: nothing is ever drawn
    bool hardware_ray_tracing =
        false;              ///< ray queries: photo mode only (ADR 0003); off with VEACH_DISABLE_RAY_TRACING=1
    bool bindless = false;  ///< Metal 3 resource IDs / SM 6.6 descriptor heaps
    bool mesh_shaders = false;
};

/// GPU time of one timed region of a frame (a render-graph pass): CommandList::begin_timing/end_timing.
struct PassTiming {
    std::string name;
    double gpu_ms = 0.0;  ///< first timestamp to last; 0 for a region that recorded no GPU work
};

/// Per-pass GPU timings of the most recently completed frame (VCH-049).
struct FrameTimings {
    std::uint64_t frame = 0;         ///< FrameInfo::index they belong to; 0 before any timed frame has completed
    std::vector<PassTiming> passes;  ///< in recording order
    /// Why `passes` is empty on this device (e.g. no timestamp sampling at encoder boundaries); empty when timed.
    std::string unavailable;
};

/// GPU memory held by one resource: buffers, textures and acceleration structures (with their scratch and instance
/// buffers), as the driver sized them.
struct ResourceMemory {
    std::string name;  ///< the resource's debug name
    std::uint64_t bytes = 0;
};

struct MemoryUsage {
    std::uint64_t device_bytes = 0;  ///< everything the device has allocated, by the driver's count; 0 if unknown
    std::vector<ResourceMemory> resources;
};

struct DeviceDesc {
    Backend backend = Backend::null;
    bool validation = false;  ///< API validation (Metal debug layer / D3D12 debug layer); errors become error logs.
    std::uint32_t frames_in_flight = 2;  ///< How far the CPU may run ahead of the GPU (ADR 0030).
};

struct SurfaceDesc {
    std::uint32_t width = 0;  ///< pixels
    std::uint32_t height = 0;
    bool vsync = true;
};

/// The format swapchain images have; render into another target and copy/draw into it.
inline constexpr Format surface_format = Format::bgra8_unorm;

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual Backend backend() const = 0;
    [[nodiscard]] virtual const Capabilities& capabilities() const = 0;

    // ---- resources
    [[nodiscard]] virtual Result<BufferHandle> create_buffer(const BufferDesc& desc,
                                                             std::span<const std::byte> initial_data = {}) = 0;
    /// `mips`, if given, holds one tightly packed span per mip level (mip_bytes each: every layer and depth slice of
    /// the level, in KTX2 order). A texture created with data is uploaded before any later submission runs and
    /// starts in shader_read. Later updates go through CommandList::copy_buffer_to_texture (render::upload_texture).
    [[nodiscard]] virtual Result<TextureHandle> create_texture(
        const TextureDesc& desc, std::span<const std::span<const std::byte>> mips = {}) = 0;
    [[nodiscard]] virtual Result<SamplerHandle> create_sampler(const SamplerDesc& desc) = 0;
    [[nodiscard]] virtual Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
    [[nodiscard]] virtual Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc& desc) = 0;
    [[nodiscard]] virtual Result<AccelHandle> create_blas(const BlasDesc& desc) = 0;
    [[nodiscard]] virtual Result<AccelHandle> create_tlas(const TlasDesc& desc) = 0;

    /// Never waits. The handle is invalid immediately; the GPU object (and, on DX12, its descriptor slots)
    /// is released once every submission made before this call has completed (ADR 0030).
    virtual void destroy(BufferHandle buffer) = 0;
    virtual void destroy(TextureHandle texture) = 0;
    virtual void destroy(SamplerHandle sampler) = 0;
    virtual void destroy(PipelineHandle pipeline) = 0;
    virtual void destroy(AccelHandle accel) = 0;

    /// Host pointer for upload/readback buffers (persistently mapped), nullptr otherwise.
    [[nodiscard]] virtual void* map(BufferHandle buffer) = 0;
    [[nodiscard]] virtual const TextureDesc* texture_desc(TextureHandle texture) const = 0;

    // ---- bindless descriptors (ADR 0027)
    [[nodiscard]] virtual DescriptorHandle descriptor(BufferHandle buffer, View view = View::read) = 0;
    /// The whole texture: its TextureType's Slang type to read (Texture3D, TextureCube...); to write, mip 0 as
    /// RWTexture2D, RWTexture3D or RWTexture2DArray (arrays and cube faces).
    [[nodiscard]] virtual DescriptorHandle descriptor(TextureHandle texture, View view = View::read) = 0;
    /// Part of a texture (ADR 0090): mips and layers from `range`. Storage views see range.base_mip only; a part of
    /// a cube reads as a 2D array. Views live as long as the texture; 0 for an invalid handle or range (logged).
    [[nodiscard]] virtual DescriptorHandle descriptor(TextureHandle texture, View view, const TextureRange& range) = 0;
    [[nodiscard]] virtual DescriptorHandle descriptor(SamplerHandle sampler) = 0;
    [[nodiscard]] virtual DescriptorHandle descriptor(AccelHandle accel) = 0;
    /// A valid descriptor of `kind` that reads as zero (ADR 0027). Use it for "no resource"; handle 0 is invalid.
    [[nodiscard]] virtual DescriptorHandle null_descriptor(DescriptorKind kind) = 0;

    // ---- surfaces (swapchains)
    [[nodiscard]] virtual Result<SurfaceId> create_surface(const NativeWindowHandle& window,
                                                           const SurfaceDesc& desc) = 0;
    virtual void resize_surface(SurfaceId surface, std::uint32_t width, std::uint32_t height) = 0;
    /// The next swapchain image (state: present). Valid until the submission that presents it.
    [[nodiscard]] virtual Result<TextureHandle> acquire_image(SurfaceId surface) = 0;
    virtual void destroy_surface(SurfaceId surface) = 0;

    // ---- frames (ADR 0030)
    /// Starts recording a frame. Blocks only until the frame that last used this frame's slot
    /// (frame - frames_in_flight) has completed on the GPU, then runs pending releases.
    virtual FrameInfo begin_frame() = 0;
    /// Ends the frame: its submissions become the fence for its slot.
    virtual void end_frame() = 0;
    [[nodiscard]] virtual FrameInfo current_frame() const = 0;
    [[nodiscard]] virtual std::uint32_t frames_in_flight() const = 0;
    /// GPU time of the most recently completed frame, in milliseconds (0 when unknown).
    [[nodiscard]] virtual double last_frame_gpu_ms() const = 0;
    /// Per-pass GPU times of the most recently completed frame. Never waits: frames still on the GPU are not
    /// included (call wait_idle() first for the latest one).
    [[nodiscard]] virtual FrameTimings last_frame_timings() = 0;
    /// The resources alive now and what they occupy.
    [[nodiscard]] virtual MemoryUsage memory_usage() = 0;

    // ---- work
    [[nodiscard]] virtual std::unique_ptr<CommandList> begin_commands(std::string_view debug_name) = 0;
    /// Submits the work; if `present` is valid, its acquired image is presented afterwards.
    virtual SubmissionId submit(std::unique_ptr<CommandList> commands, SurfaceId present = {}) = 0;
    virtual void wait(SubmissionId submission) = 0;
    /// Whether `submission`, and so every earlier one, has completed on the GPU. Never waits (VCH-073). Id 0 is
    /// complete; an id not yet submitted is not.
    [[nodiscard]] virtual bool is_complete(SubmissionId submission) = 0;
    /// Waits for all submitted work and runs every pending release.
    virtual void wait_idle() = 0;
};

[[nodiscard]] Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc);
[[nodiscard]] std::string_view to_string(Backend backend);

}  // namespace veach::rhi
