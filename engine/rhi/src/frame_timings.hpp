#pragma once

// Bookkeeping for per-pass GPU timings (VCH-049), shared by the Metal and DX12 backends. Each frame slot owns
// `capacity` timestamp samples (a Metal counter sample buffer, a D3D12 query heap). Samples come in pairs, start
// and end: Metal takes one pair per encoder, DX12 one per region. A region owns the pairs recorded while it was
// open; its time is its earliest start to its latest end. A slot is resolved once its frame has completed on the
// GPU, which begin_frame() guarantees before it reuses the slot (ADR 0029).

#include <veach/core/log.hpp>
#include <veach/rhi/rhi.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach::rhi {

class FrameTimingSlots {
public:
    FrameTimingSlots(std::uint32_t frames_in_flight, std::uint32_t capacity)
        : slots_(std::max(1u, frames_in_flight)), capacity_(capacity) {}

    [[nodiscard]] std::uint32_t capacity() const { return capacity_; }

    /// Starts frame `frame` in `slot`, forgetting what the slot held. Resolve it first (collect()).
    void begin_frame(std::uint32_t slot, std::uint64_t frame) {
        Slot& s = slots_.at(slot);
        s = Slot{};
        s.frame = frame;
    }

    /// The frame's last submission: once the GPU completes it, the slot can be resolved.
    void end_frame(std::uint32_t slot, std::uint64_t fence) {
        Slot& s = slots_.at(slot);
        s.fence = fence;
        s.ended = true;
    }

    void begin_region(std::uint32_t slot, std::string_view name) {
        Slot& s = slots_.at(slot);
        s.regions.push_back({std::string(name), s.used, s.used});
        s.open = true;
    }

    void end_region(std::uint32_t slot) {
        Slot& s = slots_.at(slot);
        if (s.open && !s.regions.empty()) {
            s.regions.back().end = s.used;
        }
        s.open = false;
    }

    [[nodiscard]] bool region_open(std::uint32_t slot) const { return slots_.at(slot).open; }

    /// Two samples (start, end) for the open region: the index of the first, or nothing when the slot is full.
    [[nodiscard]] std::optional<std::uint32_t> allocate_pair(std::uint32_t slot) {
        Slot& s = slots_.at(slot);
        if (s.used + 2 > capacity_) {
            if (!s.overflowed) {
                log::warn("rhi", "frame {} has more GPU-timed work than {} timestamps; later passes report 0 ms",
                          s.frame, capacity_);
            }
            s.overflowed = true;
            return std::nullopt;
        }
        const std::uint32_t first = s.used;
        s.used += 2;
        if (s.open && !s.regions.empty()) {
            s.regions.back().end = s.used;
        }
        return first;
    }

    /// Samples used so far in `slot` (what a backend must resolve).
    [[nodiscard]] std::uint32_t used(std::uint32_t slot) const { return slots_.at(slot).used; }

    /// Resolves every ended slot whose last submission is `<= completed` and publishes the newest frame.
    /// `read(slot, count)` returns the slot's first `count` timestamps; `to_ms` turns a tick span into ms.
    /// Timestamps equal to `invalid` (a sample the GPU did not write) are skipped.
    template <class Read, class ToMs>
    void collect(std::uint64_t completed, const Read& read, const ToMs& to_ms,
                 std::uint64_t invalid = std::numeric_limits<std::uint64_t>::max()) {
        for (std::uint32_t index = 0; index < slots_.size(); ++index) {
            Slot& s = slots_[index];
            if (!s.ended || s.resolved || s.fence > completed) {
                continue;
            }
            s.resolved = true;
            if (s.frame <= published_.frame) {
                continue;
            }
            const std::vector<std::uint64_t> stamps = s.used > 0 ? read(index, s.used) : std::vector<std::uint64_t>{};
            FrameTimings out;
            out.frame = s.frame;
            std::uint64_t frame_first = invalid;
            std::uint64_t frame_last = 0;
            for (const Region& r : s.regions) {
                std::uint64_t first = invalid;
                std::uint64_t last = 0;
                for (std::uint32_t i = r.first; i + 1 < r.end && i + 1 < stamps.size(); i += 2) {
                    if (stamps[i] != invalid && stamps[i + 1] != invalid && stamps[i + 1] >= stamps[i]) {
                        first = std::min(first, stamps[i]);
                        last = std::max(last, stamps[i + 1]);
                    }
                }
                double ms = 0.0;
                if (first != invalid && last >= first) {
                    ms = to_ms(last - first);
                    frame_first = std::min(frame_first, first);
                    frame_last = std::max(frame_last, last);
                }
                out.passes.push_back({r.name, ms});
            }
            published_ = std::move(out);
            published_span_ms_ =
                frame_first != invalid && frame_last >= frame_first ? to_ms(frame_last - frame_first) : 0.0;
        }
    }

    [[nodiscard]] const FrameTimings& published() const { return published_; }
    /// The newest resolved frame's first timestamp to its last (the timed part of the frame), in ms.
    [[nodiscard]] double published_span_ms() const { return published_span_ms_; }

private:
    struct Region {
        std::string name;
        std::uint32_t first = 0;  ///< sample range [first, end) of (start, end) pairs
        std::uint32_t end = 0;
    };
    struct Slot {
        std::uint64_t frame = 0;
        std::uint64_t fence = 0;
        bool ended = false;
        bool resolved = false;
        bool open = false;
        bool overflowed = false;
        std::uint32_t used = 0;
        std::vector<Region> regions;
    };

    std::vector<Slot> slots_;
    std::uint32_t capacity_ = 0;
    FrameTimings published_;
    double published_span_ms_ = 0.0;
};

}  // namespace veach::rhi
