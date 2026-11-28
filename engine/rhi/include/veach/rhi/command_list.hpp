#pragma once

// Command recording. One CommandList per submission. Passes are explicit:
// draws happen inside begin/end_render_pass, dispatches inside
// begin/end_compute_pass; copies and acceleration-structure builds happen
// outside both. Barriers and residency come from the render graph (ADR 0029).

#include <veach/rhi/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

namespace veach::rhi {

/// DX12 root constants: 60 of the root signature's 64 DWORDs (ADR 0027); the other 4 hold the descriptor-validation
/// buffers (ADR 0027).
inline constexpr std::size_t max_parameter_bytes = 240;

class CommandList {
public:
    virtual ~CommandList() = default;

    virtual void begin_render_pass(const RenderPassDesc& desc) = 0;
    virtual void end_render_pass() = 0;
    virtual void begin_compute_pass(std::string_view debug_name) = 0;
    virtual void end_compute_pass() = 0;

    virtual void set_pipeline(PipelineHandle pipeline) = 0;
    /// The parameter block for the next draws/dispatches (Slang `ConstantBuffer<Params>`), at most 256 bytes.
    virtual void set_parameters(const void* data, std::size_t size) = 0;
    virtual void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1, std::uint32_t first_vertex = 0) = 0;
    virtual void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z = 1) = 0;

    virtual void build_blas(AccelHandle blas) = 0;
    virtual void refit_blas(AccelHandle blas) = 0;
    virtual void build_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) = 0;
    /// Updates transforms in place. Same instance count and BLAS assignments as the last build.
    virtual void refit_tlas(AccelHandle tlas, std::span<const TlasInstance> instances) = 0;

    /// Copies a whole texture into a buffer, tightly packed rows.
    virtual void copy_texture_to_buffer(TextureHandle src, BufferHandle dst) = 0;
    /// Copies the first `size` bytes of `src` to the start of `dst` (copy_src and copy_dst states).
    virtual void copy_buffer(BufferHandle src, BufferHandle dst, std::uint64_t size) = 0;
    /// Copies texels from `src` into `region` of `dst` (copy_src and copy_dst states; VCH-076). The data starts at
    /// `offset` (a multiple of texture_copy_offset_alignment), rows are `row_pitch` bytes apart (a multiple of
    /// texture_copy_row_alignment, at least row_bytes of the region), depth slices are row_pitch x row_count(height)
    /// bytes apart, and layers texture_copy_layer_stride apart. render::upload_texture stages data this way.
    virtual void copy_buffer_to_texture(BufferHandle src, std::uint64_t offset, std::uint32_t row_pitch,
                                        TextureHandle dst, const TextureRegion& region) = 0;

    /// Declares that the current pass accesses `resource` in `state` (residency on Metal).
    virtual void use(ResourceRef resource, ResourceState state) = 0;
    virtual void barrier(std::span<const Barrier> barriers) = 0;

    virtual void push_debug_group(std::string_view name) = 0;
    virtual void pop_debug_group() = 0;

    /// GPU timing (VCH-049): the work recorded until end_timing() is timed with GPU timestamps and reported as
    /// `name` by Device::last_frame_timings() once its frame has completed. Regions do not nest. On Metal a
    /// region times whole encoders, so call begin_timing before begin_render_pass/begin_compute_pass. The
    /// render graph times every pass this way.
    virtual void begin_timing(std::string_view name) = 0;
    virtual void end_timing() = 0;

    template <class T>
    void set_parameters(const T& params) {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= max_parameter_bytes);
        set_parameters(&params, sizeof(T));
    }
};

}  // namespace veach::rhi
