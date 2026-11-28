#pragma once

// Updating textures after creation (VCH-076, ADR 0090): texel data is staged in an upload ring, then copied into
// the texture by a transfer pass of the render graph, which derives the barriers like any other access.
//
//   UploadRing& ring = *frame.uploads;                 // or one of your own
//   auto texture = graph.import(chunk_heights_, "heights", rhi::ResourceState::shader_read);
//   auto done = render::upload_texture(graph, ring, texture, {.mip = 0, .x = 64, .y = 0, .width = 64, .height = 64},
//                                      bytes, "upload heights");
//   // later passes read `texture` in shader_read as usual

#include <veach/core/error.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/rhi/rhi.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace veach::render {

/// Upload memory recycled frame by frame: one share of a persistently mapped buffer per frame in flight. A share is
/// written again only once its frame slot comes round, after Device::begin_frame has waited for the GPU to finish
/// the frame that last used it (ADR 0030), so data staged in a frame stays valid until that frame's copies ran.
/// A request bigger than what is left of the share gets a buffer of its own, destroyed when the slot comes round.
class UploadRing {
public:
    static constexpr std::uint64_t default_bytes_per_frame = 8ull << 20;

    explicit UploadRing(rhi::Device& device, std::uint64_t bytes_per_frame = default_bytes_per_frame);
    ~UploadRing();
    UploadRing(const UploadRing&) = delete;
    UploadRing& operator=(const UploadRing&) = delete;
    UploadRing(UploadRing&&) = delete;
    UploadRing& operator=(UploadRing&&) = delete;

    struct Allocation {
        rhi::BufferHandle buffer;
        std::uint64_t offset = 0;   ///< into `buffer`
        std::byte* data = nullptr;  ///< CPU address of `offset`: write `size` bytes here
    };

    /// `size` bytes of this frame's upload memory at an `alignment`-aligned offset (a power of two), valid until
    /// this frame slot comes round again. The ring's buffer is created on first use.
    [[nodiscard]] Result<Allocation> allocate(std::uint64_t size,
                                              std::uint64_t alignment = rhi::texture_copy_offset_alignment);

    [[nodiscard]] std::uint64_t bytes_per_frame() const { return bytes_per_frame_; }
    /// Bytes of the current frame's share in use, and of the buffers made for requests beyond it.
    [[nodiscard]] std::uint64_t used() const;
    [[nodiscard]] std::uint64_t overflow() const;

private:
    struct Slot {
        std::uint64_t frame = UINT64_MAX;  ///< FrameInfo::index that last allocated from this slot
        std::uint64_t head = 0;
        std::uint64_t overflow_bytes = 0;
        std::vector<rhi::BufferHandle> overflow;
    };
    Slot& current();

    rhi::Device& device_;
    std::uint64_t bytes_per_frame_;
    rhi::BufferHandle buffer_;
    std::byte* mapped_ = nullptr;
    std::vector<Slot> slots_;
};

/// Stages `data` in `ring` and adds a transfer pass, `pass_name`, that copies it into `region` of `texture` (copy_dst
/// for the pass; the graph moves it there and back to whatever later passes declare). `data` is tightly packed in
/// KTX2 order (layers, then depth slices, then rows of texels or 4x4 blocks): rhi::mip_bytes for a whole mip. A
/// region with zero extents reaches the edge of the mip. Regions uploaded to one texture in one frame must not
/// overlap. Errors: rhi.invalid_texture (a region outside the texture, or data of the wrong size).
[[nodiscard]] Result<> upload_texture(RenderGraph& graph, UploadRing& ring, GraphResource texture,
                                      const rhi::TextureRegion& region, std::span<const std::byte> data,
                                      std::string pass_name = "texture upload");

/// Tightly packed bytes of a resolved region: row_bytes x row_count x depth x layers.
[[nodiscard]] std::uint64_t region_bytes(rhi::Format format, const rhi::TextureRegion& region);

}  // namespace veach::render
