#pragma once

// Terrain edits (docs/adr/0054-terrain-edits-and-water.md, docs/adr/0077-amendments-from-sp-12.md,
// docs/specs/voxel-terrain/spec.md): the brush operation, its integer applier and the edit store. Promoted from
// spike SP-12 (docs/research/spikes/sp-12-dig-collide.md). Canonical (ADR 0043): integers only, so an op log replays
// to the same samples on every platform, with any number of workers, in any chunk order.
//
// The new value of a sample is a function of its old value, its lattice position and the op only, so every chunk
// holding a point computes the same value for it. Ops must be applied in sequence order (add and subtract don't
// commute); the chunks of one op may be processed in parallel.
//
// The EditStore keeps one override record per edited LOD0 chunk with the samples it owns (0 to 31 on each axis, so
// each lattice point is stored once, ADR 0077 rule 2) and a mask of the ones an edit wrote. Any chunk of any level
// reads them through overlay(): its samples sit on LOD0 lattice points, so a coarse chunk takes the override at each
// of its points that an edit wrote (ADR 0054's exact coarse derivation; an edit smaller than a coarse cell may fall
// between its points and not show there).

#include <veach/core/error.hpp>
#include <veach/voxel/voxel.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace veach {
class JobSystem;
}

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
/// The same on samples encoded with `encoding`: where the brush writes a distance beyond the companded knee (16 m)
/// it is encoded as the samples are (ADR 0079 rule 1). Linear is apply_brush above.
std::size_t apply_brush(const BrushOp& op, ChunkSamples& samples, OverrideMask& mask, DistanceEncoding encoding);

/// Integer-only hash of a chunk's samples (key, every grid distance and material), canonical: ADR 0054's chunk hash.
std::uint64_t samples_hash(const ChunkSamples& samples);

/// floor(sqrt(v)), exact, integer only.
std::uint64_t isqrt(std::uint64_t v);

/// Checks an op against ADR 0054's rules: a known kind at version 1, a sphere, 1 mm to max_brush_radius_mm.
/// Fails with voxel.brush_invalid.
[[nodiscard]] Result<> validate(const BrushOp& op);

class DensitySource;

/// One edited LOD0 chunk (ADR 0054's override record): the chunk's owned samples, 0 to 31 on each axis, after the
/// generator and every op, and which of them an op wrote. At rest only the masked samples matter; `base_hash` hashes
/// the generated samples the record was made on.
struct OverrideRecord {
    static constexpr int owned_n = chunk_cells;
    static constexpr std::size_t owned_count = std::size_t{owned_n} * owned_n * owned_n;
    ChunkKey key;
    std::uint64_t base_hash = 0;
    std::vector<Sample> samples;     ///< owned_count, x fastest
    std::vector<std::uint8_t> mask;  ///< owned_count; 1 = written by an op
    std::size_t overridden = 0;      ///< samples with mask 1

    static constexpr std::size_t index(int x, int y, int z) {
        return static_cast<std::size_t>(x + owned_n * (y + owned_n * z));
    }
};

/// Hash of an override record: key, base hash, and every masked sample with its index (canonical).
[[nodiscard]] std::uint64_t record_hash(const OverrideRecord& record);
/// Hash of the generated owned samples (OverrideRecord::base_hash).
[[nodiscard]] std::uint64_t base_hash(const ChunkKey& key, std::span<const Sample> owned);

/// What one op did.
struct EditResult {
    std::uint64_t seq = 0;
    LatticeBox box;                 ///< brush_box(op): the lattice points it may have written
    std::vector<ChunkKey> changed;  ///< LOD0 chunks whose owned samples changed, by key
    std::size_t samples_changed = 0;
    std::size_t materialized = 0;  ///< chunks sampled for the first time (ADR 0077 rule 3)
};

/// The edits of one terrain body: override records and the journal of ops since the last fold.
class EditStore {
public:
    explicit EditStore(DistanceEncoding encoding = DistanceEncoding::companded) : encoding_(encoding) {}

    /// Applies `op` to the owned samples of every LOD0 chunk it may write, one chunk per job, sampling a chunk from
    /// `source` the first time an op touches it (even one the bounds proved empty or solid). A `seq` of 0 takes the
    /// next number; otherwise it must be larger than the last. Records the op in the journal. Fails with
    /// voxel.brush_invalid and changes nothing.
    [[nodiscard]] Result<EditResult> apply(BrushOp op, const DensitySource& source, JobSystem& jobs);

    /// Writes the overrides into a chunk's samples (grid, apron and faces, any level): every sample on a lattice
    /// point an op wrote takes the override. Updates has_surface. Thread-safe against other const calls.
    void overlay(ChunkSamples& samples) const;
    /// True if some record's owned samples lie in `box` (the box may hold edited samples).
    [[nodiscard]] bool touches(const LatticeBox& box) const;

    [[nodiscard]] const OverrideRecord* find(const ChunkKey& key) const;
    [[nodiscard]] const std::map<ChunkKey, OverrideRecord>& records() const { return records_; }
    [[nodiscard]] const std::vector<BrushOp>& journal() const { return journal_; }
    /// Hands over the journal (a save completed: the records now hold its effect) and clears it.
    [[nodiscard]] std::vector<BrushOp> fold_journal();
    [[nodiscard]] std::uint64_t last_seq() const { return last_seq_; }
    /// record_hash of every record, folded in key order.
    [[nodiscard]] std::uint64_t hash() const;
    /// Overridden samples across records (each lattice point once).
    [[nodiscard]] std::size_t override_samples() const;

    /// Puts back a record loaded from a save: its masked samples over freshly generated ones. Returns false, and
    /// logs voxel.base_mismatch, if the regenerated base does not hash to record.base_hash; the overrides apply
    /// either way (they are absolute).
    bool restore(const OverrideRecord& record, const DensitySource& source);

private:
    DistanceEncoding encoding_;
    std::map<ChunkKey, OverrideRecord> records_;
    std::vector<BrushOp> journal_;
    std::uint64_t last_seq_ = 0;
};

}  // namespace veach::voxel
