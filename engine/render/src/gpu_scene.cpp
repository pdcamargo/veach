#include "gpu_scene.hpp"

#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace veach::render {

namespace {

constexpr std::uint32_t record_size = sizeof(InstanceRecord);
/// Changed records closer than this are uploaded as one run (fewer copy regions, a little more data).
constexpr std::uint32_t coalesce_gap = 8;

}  // namespace

GpuScene::~GpuScene() {
    if (buffer_.is_valid()) {
        device_.destroy(buffer_);
    }
}

GraphResource GpuScene::update(RenderGraph& graph, UploadRing& ring, std::vector<InstanceRecord>& records,
                               std::span<const std::uint64_t> entities, const RenderSpace& space) {
    VEACH_ZONE("GpuScene::update");
    const auto count = static_cast<std::uint32_t>(records.size());
    uploaded_bytes_ = 0;

    // The buffer grows to twice what it needs; everything is uploaded into a new one.
    bool everything = !written_;
    if (count > capacity_ || !buffer_.is_valid()) {
        if (buffer_.is_valid()) {
            device_.destroy(buffer_);  // deferred until the frames in flight are done with it
            buffer_ = {};
        }
        const std::uint32_t capacity = std::max<std::uint32_t>(64, count * 2);
        auto buffer = device_.create_buffer({.size = std::uint64_t{capacity} * record_size,
                                             .usage = rhi::BufferUsage::storage | rhi::BufferUsage::copy_dst,
                                             .debug_name = "GPU scene instances",
                                             .stride = record_size});
        if (!buffer) {
            log::error("render", buffer.error());
            capacity_ = 0;
            records_.clear();
            entities_.clear();
            return {};
        }
        buffer_ = *buffer;
        capacity_ = capacity;
        written_ = false;
        everything = true;
    }

    // One pass over the records (a field of foliage has hundreds of thousands): each gets its entity's transform of
    // last frame, moved from last frame's render origin to this one's (a whole number of 64 m cells, exact in float;
    // a change of render frame starts over without motion), then is compared with what the buffer holds. Changed
    // records closer than coalesce_gap make one run.
    const bool same_space = has_space_ && space_.frame == space.frame;
    const math::vec3 shift = same_space ? math::vec3(space_.origin - space.origin) : math::vec3(0.0f);
    index_built_ = false;
    struct Run {
        std::uint32_t first;
        std::uint32_t count;
    };
    std::vector<Run> runs;
    std::uint32_t changed = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        InstanceRecord& r = records[i];
        r.previous_object_to_world = r.object_to_world;
        const std::uint64_t entity = entities[i];
        if (same_space && entity != 0) {
            const InstanceRecord* before = nullptr;
            if (i < entities_.size() && entities_[i] == entity) {
                before = &records_[i];
            } else {
                if (!index_built_) {
                    index_.clear();
                    for (std::uint32_t k = 0; k < entities_.size(); ++k) {
                        index_.emplace(entities_[k], k);
                    }
                    index_built_ = true;
                }
                if (const auto it = index_.find(entity); it != index_.end()) {
                    before = &records_[it->second];
                }
            }
            if (before != nullptr) {
                r.previous_object_to_world = before->object_to_world;
                for (std::size_t row = 0; row < 3; ++row) {
                    r.previous_object_to_world.at(row).w += shift[static_cast<math::length_t>(row)];
                }
            }
        }
        if (!everything && i < records_.size() && std::memcmp(&r, &records_[i], record_size) == 0) {
            continue;
        }
        if (!runs.empty() && i - (runs.back().first + runs.back().count) <= coalesce_gap) {
            changed += i + 1 - (runs.back().first + runs.back().count);
            runs.back().count = i + 1 - runs.back().first;
        } else {
            runs.push_back({i, 1});
            ++changed;
        }
    }
    space_ = space;
    has_space_ = true;
    // The mirror of the buffer: only the changed runs are copied (lookups above needed last frame's records).
    records_.resize(count);
    for (const Run& run : runs) {
        std::memcpy(&records_[run.first], &records[run.first], std::size_t{run.count} * record_size);
    }
    entities_.assign(entities.begin(), entities.end());

    const GraphResource resource = graph.import(
        buffer_, "GPU scene instances", written_ ? rhi::ResourceState::shader_read : rhi::ResourceState::undefined,
        rhi::ResourceState::shader_read);
    if (runs.empty()) {
        written_ = written_ || count == 0;
        return resource;
    }
    auto staging = ring.allocate(std::uint64_t{changed} * record_size, 16);
    if (!staging) {
        log::error("render", staging.error());
        records_.clear();  // upload everything next frame
        entities_.clear();
        return {};
    }
    std::vector<rhi::BufferCopy> regions;
    std::uint64_t offset = 0;
    for (const Run& run : runs) {
        const std::uint64_t bytes = std::uint64_t{run.count} * record_size;
        std::memcpy(staging->data + offset, &records[run.first], bytes);
        regions.push_back({staging->offset + offset, std::uint64_t{run.first} * record_size, bytes});
        offset += bytes;
    }
    uploaded_bytes_ = offset;
    written_ = true;
    const GraphResource source = graph.import(staging->buffer, "GPU scene upload", rhi::ResourceState::copy_src);
    graph.add_transfer_pass(
        "GPU scene upload",
        [&](PassBuilder& b) {
            b.read(source, rhi::ResourceState::copy_src);
            b.write(resource, rhi::ResourceState::copy_dst);
        },
        [source, resource, regions = std::move(regions)](PassContext& ctx) {
            ctx.cmd.copy_buffer_regions(ctx.buffer(source), ctx.buffer(resource), regions);
        });
    return resource;
}

}  // namespace veach::render
