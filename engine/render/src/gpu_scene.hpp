#pragma once

// The GPU scene (docs/adr/0093-gpu-driven-core.md): every drawn mesh's InstanceRecord in one persistent GPU buffer that
// culling, the visibility pass, shading and photo mode read. Extraction rebuilds the records each frame in render
// space (camera-relative, ADR 0045); only the records that changed are uploaded, through the upload ring and one
// copy pass, so a static scene uploads nothing while the camera moves inside its 64 m render cell. Each record also
// carries its entity's transform of the frame before, moved into this frame's render space, for motion vectors.

#include <veach/core/error.hpp>
#include <veach/render/render_graph.hpp>
#include <veach/render/renderer.hpp>
#include <veach/render/upload.hpp>

#include "scene_records.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace veach::render {

class GpuScene {
public:
    explicit GpuScene(rhi::Device& device) : device_(device) {}
    ~GpuScene();
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;
    GpuScene(GpuScene&&) = delete;
    GpuScene& operator=(GpuScene&&) = delete;

    /// This frame's records in draw order, each from `entities[i]` (0: no entity, no motion). Fills every record's
    /// previous_object_to_world from the same entity's record last frame (its current transform when there is none,
    /// or when render space changed frame), uploads the records that differ from last frame's, and returns the buffer
    /// as a graph resource that later passes read in shader_read. Invalid on failure (logged).
    [[nodiscard]] GraphResource update(RenderGraph& graph, UploadRing& ring, std::vector<InstanceRecord>& records,
                                       std::span<const std::uint64_t> entities, const RenderSpace& space);

    [[nodiscard]] rhi::BufferHandle buffer() const { return buffer_; }
    /// Bytes the last update() uploaded (0 for a frame where nothing changed).
    [[nodiscard]] std::uint64_t uploaded_bytes() const { return uploaded_bytes_; }
    /// Whether the last update() found every record as the frame before left it, in the same render space: nothing
    /// in the scene moved, came or went (what a cached shadow cascade needs to stay valid).
    [[nodiscard]] bool still() const { return still_; }

private:
    rhi::Device& device_;
    rhi::BufferHandle buffer_;
    std::uint32_t capacity_ = 0;
    bool written_ = false;
    std::vector<InstanceRecord> records_;                     ///< what the buffer holds: last frame's records
    std::vector<std::uint64_t> entities_;                     ///< and their entities
    std::unordered_map<std::uint64_t, std::uint32_t> index_;  ///< entity -> index in records_, built on demand
    bool index_built_ = false;
    RenderSpace space_;
    bool has_space_ = false;
    std::uint64_t uploaded_bytes_ = 0;
    bool still_ = false;
};

}  // namespace veach::render
