#pragma once

// veach::voxel edits, spike SP-12 (VCH-088): ADR 0054's brush operation and its integer applier
// (docs/research/spikes/sp-12-dig-collide.md). Canonical (ADR 0043): integers only, so an op log replays to the same
// samples on every platform, with any number of workers, in any chunk order.
//
// An op is applied to every LOD0 chunk whose sample grid (apron included) it touches, each chunk independently: the
// new value of a sample is a function of its old value, its lattice position and the op only. Neighboring chunks
// hold the same samples at shared positions, so they stay equal after every op. Ops must be applied in sequence
// order (add and subtract don't commute); the chunks of one op may be processed in parallel.

#include <veach/voxel/voxel.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace veach::voxel {

/// ADR 0054's kinds. SP-12 implements the two CSG kinds a dig needs; flatten, smooth, raise, lower and paint join
/// this enum, with their record version, when they are built.
enum class BrushKind : std::uint8_t { add = 0, subtract = 1 };
enum class BrushShape : std::uint8_t { sphere = 0 };

/// One brush operation (ADR 0054), the unit on the wire and in the journal. Positions are integer body-local
/// millimeters; nothing here is a float. Fields ADR 0054 lists that no SP-12 kind reads yet (orientation, strength,
/// falloff, author) are added with the kinds that read them.
struct BrushOp {
    std::uint16_t version = 1;  ///< record version of `kind` (`kind@version`)
    BrushKind kind = BrushKind::subtract;
    BrushShape shape = BrushShape::sphere;
    std::array<std::int64_t, 3> center_mm{};  ///< body-local, body-fixed frame
    std::int32_t radius_mm = 0;               ///< at most max_brush_radius_mm
    std::uint8_t material = 0;                ///< written by `add` where the brush becomes the surface
    std::uint64_t tick = 0;                   ///< the fixed step the op was committed at
    std::uint64_t seq = 0;                    ///< per-body sequence number, assigned by the server
};

/// ADR 0054's per-op size cap (working default 16 m, pending the owner's review).
inline constexpr std::int32_t max_brush_radius_mm = 16'000;
/// Millimeters per LOD0 lattice unit (0.5 m).
inline constexpr std::int64_t lod0_cell_mm = 500;
/// How far past the brush surface samples are rewritten, in LOD0 cells. Inside it the result is the exact CSG of the
/// two distance fields, so the mesher's gradients (one sample beyond each crossing edge) see true distances;
/// beyond it samples keep their old, farther value with the same sign.
inline constexpr std::int64_t brush_margin_cells = 2;

/// Lattice box (LOD0 units, inclusive) of the samples `op` may write.
struct LatticeBox {
    std::array<std::int64_t, 3> lo{}, hi{};
};
LatticeBox brush_box(const BrushOp& op);

/// The LOD0 chunks whose sample grid (-1..33 on each axis, the apron included) meets brush_box(op), sorted by key.
std::vector<ChunkKey> touched_chunks(const BrushOp& op);

/// Per-sample override mask of one chunk's grid (ChunkSamples::grid_n³ entries, 1 = written by an edit). With the
/// overridden samples it is ADR 0054's override record at rest; the samples live in the chunk's ChunkSamples.
using OverrideMask = std::vector<std::uint8_t>;

/// Applies `op` to one LOD0 chunk's samples (generated, then every earlier op), in integer arithmetic, and marks the
/// samples whose value changed in `mask` (resized on first use). Recomputes `samples.has_surface`. Returns the
/// number of samples changed. The chunk must be LOD0 and fully sampled.
std::size_t apply_brush(const BrushOp& op, ChunkSamples& samples, OverrideMask& mask);

/// Integer-only hash of a chunk's samples (key, every grid distance and material), canonical: ADR 0054's chunk hash.
std::uint64_t samples_hash(const ChunkSamples& samples);

/// floor(sqrt(v)), exact, integer only.
std::uint64_t isqrt(std::uint64_t v);

}  // namespace veach::voxel
