// veach: canonical
// Brush operations applied to LOD0 samples in integer arithmetic, and the edit store (edit.hpp; ADR 0054, 0077;
// promoted from spike SP-12, docs/research/spikes/sp-12-dig-collide.md).

#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/voxel/density.hpp>
#include <veach/voxel/edit.hpp>

#include <algorithm>
#include <format>

namespace veach::voxel {
namespace {

constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}
constexpr std::int64_t ceil_div(std::int64_t a, std::int64_t b) {
    return -floor_div(-a, b);
}
/// a / b rounded to nearest, halves up, for b > 0.
constexpr std::int64_t round_div(std::int64_t a, std::int64_t b) {
    return floor_div(2 * a + b, 2 * b);
}

std::int64_t reach_mm(const BrushOp& op) {
    return std::int64_t{op.radius_mm} + brush_margin_cells * lod0_cell_mm;
}

/// The code of a brush distance given in 1/1024 m: the linear code itself within the knee, the encoding's beyond.
std::int64_t brush_code(std::int64_t distance_1024, DistanceEncoding encoding) {
    if (encoding == DistanceEncoding::linear || (distance_1024 <= 16384 && distance_1024 >= -16384)) {
        return std::clamp<std::int64_t>(distance_1024, -32767, 32767);
    }
    return encode_distance(static_cast<double>(distance_1024) / 1024.0, encoding);
}

/// The brush at one lattice point: the sample's new value. False if the point is beyond the brush's reach.
struct Kernel {
    const BrushOp& op;
    DistanceEncoding encoding;
    std::int64_t reach2;
    std::int64_t radius_1024;

    Kernel(const BrushOp& o, DistanceEncoding e)
        : op(o), encoding(e), reach2(reach_mm(o) * reach_mm(o)), radius_1024(std::int64_t{o.radius_mm} * 1024) {}

    /// Applies the op to `sample` at lattice point p; true if it changed.
    bool apply(std::int64_t px, std::int64_t py, std::int64_t pz, Sample& sample) const {
        const std::int64_t dx = px * lod0_cell_mm - op.center_mm[0];
        const std::int64_t dy = py * lod0_cell_mm - op.center_mm[1];
        const std::int64_t dz = pz * lod0_cell_mm - op.center_mm[2];
        const std::int64_t d2 = dx * dx + dy * dy + dz * dz;  // mm², at most 3 · 17 m² · 10⁶
        if (d2 > reach2) {
            return false;
        }
        // Distance to the brush surface in 1/1024 m, positive outside: 1024 · |p − c| is isqrt(d2 · 1024²) (floor),
        // then (· − 1024 r) / 1000 mm per m, rounded.
        const auto scaled = static_cast<std::int64_t>(isqrt(static_cast<std::uint64_t>(d2) << 20));
        const std::int64_t brush = round_div(scaled - radius_1024, 1000);
        const std::int64_t old = sample.distance;
        std::int64_t value = old;
        std::uint8_t material = sample.material;
        if (op.kind == BrushKind::subtract) {
            value = std::max(old, brush_code(-brush, encoding));
        } else if (const std::int64_t code = brush_code(brush, encoding); code < old) {
            value = code;
            material = op.material;
        }
        value = std::clamp<std::int64_t>(value, -32767, 32767);
        if (value == old && material == sample.material) {
            return false;
        }
        sample.distance = static_cast<std::int16_t>(value);
        sample.material = material;
        return true;
    }
};

std::uint64_t absorb(std::uint64_t h, std::uint64_t w) {
    return mix64(h ^ mix64(w + hash_golden));
}

std::uint64_t sample_word(const Sample& v) {
    return static_cast<std::uint16_t>(v.distance) | (std::uint64_t{v.material} << 16);
}

}  // namespace

std::uint64_t isqrt(std::uint64_t v) {
    std::uint64_t r = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

LatticeBox brush_box(const BrushOp& op) {
    const std::int64_t reach = reach_mm(op);
    LatticeBox box;
    for (std::size_t a = 0; a < 3; ++a) {
        box.lo[a] = ceil_div(op.center_mm[a] - reach, lod0_cell_mm);
        box.hi[a] = floor_div(op.center_mm[a] + reach, lod0_cell_mm);
    }
    return box;
}

std::vector<ChunkKey> touched_chunks(const BrushOp& op) {
    const LatticeBox box = brush_box(op);
    std::array<std::int64_t, 3> lo{}, hi{};
    for (std::size_t a = 0; a < 3; ++a) {
        // Chunk k's grid covers lattice points 32k - 1 .. 32k + 33.
        lo[a] = ceil_div(box.lo[a] - (chunk_cells + 1), chunk_cells);
        hi[a] = floor_div(box.hi[a] + 1, chunk_cells);
    }
    std::vector<ChunkKey> keys;
    for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
        for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
                keys.push_back(ChunkKey{0, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                        static_cast<std::int32_t>(z)});
            }
        }
    }
    return keys;  // x, then y, then z ascending: ChunkKey's order
}

std::size_t apply_brush(const BrushOp& op, ChunkSamples& s, OverrideMask& mask) {
    return apply_brush(op, s, mask, DistanceEncoding::linear);
}

std::size_t apply_brush(const BrushOp& op, ChunkSamples& s, OverrideMask& mask, DistanceEncoding encoding) {
    constexpr int n = ChunkSamples::grid_n;
    if (mask.size() != static_cast<std::size_t>(n) * n * n) {
        mask.assign(static_cast<std::size_t>(n) * n * n, 0);
    }
    const LatticeBox box = brush_box(op);
    const auto o = chunk_origin(s.key);
    std::array<int, 3> lo{}, hi{};
    for (std::size_t a = 0; a < 3; ++a) {
        lo[a] = static_cast<int>(std::max<std::int64_t>(-1, box.lo[a] - o[a]));
        hi[a] = static_cast<int>(std::min<std::int64_t>(chunk_cells + 1, box.hi[a] - o[a]));
        if (lo[a] > hi[a]) {
            return 0;
        }
    }
    const Kernel kernel(op, encoding);
    std::size_t changed = 0;
    for (int z = lo[2]; z <= hi[2]; ++z) {
        for (int y = lo[1]; y <= hi[1]; ++y) {
            for (int x = lo[0]; x <= hi[0]; ++x) {
                if (kernel.apply(o[0] + x, o[1] + y, o[2] + z, s.at(x, y, z))) {
                    mask[ChunkSamples::grid_index(x, y, z)] = 1;
                    ++changed;
                }
            }
        }
    }
    if (changed != 0) {
        bool neg = false, pos = false;
        for (const Sample& v : s.grid) {
            (v.distance < 0 ? neg : pos) = true;
        }
        s.has_surface = neg && pos;
    }
    return changed;
}

std::uint64_t samples_hash(const ChunkSamples& s) {
    std::uint64_t h = hash(hash_id("veach:voxel.samples"), s.key.level, static_cast<std::uint32_t>(s.key.x),
                           static_cast<std::uint32_t>(s.key.y), static_cast<std::uint32_t>(s.key.z), s.grid.size());
    for (const Sample& v : s.grid) {
        h = mix64(h ^ mix64(sample_word(v) + hash_golden));
    }
    return h;
}

Result<> validate(const BrushOp& op) {
    if (op.version != 1 || (op.kind != BrushKind::add && op.kind != BrushKind::subtract)) {
        return make_error(ErrorCode::voxel_brush_invalid,
                          std::format("brush kind {}@{} is not known", static_cast<int>(op.kind), op.version),
                          "This build applies add@1 and subtract@1 (ADR 0054); the other kinds come with their "
                          "record versions.");
    }
    if (op.shape != BrushShape::sphere) {
        return make_error(ErrorCode::voxel_brush_invalid,
                          std::format("brush shape {} is not known", static_cast<int>(op.shape)),
                          "This build has the sphere shape only.");
    }
    if (op.radius_mm <= 0 || op.radius_mm > max_brush_radius_mm) {
        return make_error(
            ErrorCode::voxel_brush_invalid, std::format("a brush radius of {} mm is out of range", op.radius_mm),
            std::format("Radii are 1 to {} mm; make a larger edit from several ops.", max_brush_radius_mm));
    }
    return {};
}

std::uint64_t base_hash(const ChunkKey& key, std::span<const Sample> owned) {
    std::uint64_t h = hash(hash_id("veach:voxel.base"), key.level, static_cast<std::uint32_t>(key.x),
                           static_cast<std::uint32_t>(key.y), static_cast<std::uint32_t>(key.z), owned.size());
    for (const Sample& v : owned) {
        h = absorb(h, sample_word(v));
    }
    return h;
}

std::uint64_t record_hash(const OverrideRecord& r) {
    std::uint64_t h = hash(hash_id("veach:voxel.override"), r.key.level, static_cast<std::uint32_t>(r.key.x),
                           static_cast<std::uint32_t>(r.key.y), static_cast<std::uint32_t>(r.key.z), r.base_hash);
    for (std::size_t i = 0; i < r.mask.size(); ++i) {
        if (r.mask[i] != 0) {
            h = absorb(h, (std::uint64_t{i} << 24) | sample_word(r.samples[i]));
        }
    }
    return absorb(h, r.overridden);
}

namespace {

/// The owned samples of a LOD0 chunk, generated (no edits).
void generate_owned(const DensitySource& source, const ChunkKey& key, OverrideRecord& r) {
    ChunkSamples s;
    source.sample(key, 0, s);
    r.samples.resize(OverrideRecord::owned_count);
    for (int z = 0; z < OverrideRecord::owned_n; ++z) {
        for (int y = 0; y < OverrideRecord::owned_n; ++y) {
            for (int x = 0; x < OverrideRecord::owned_n; ++x) {
                r.samples[OverrideRecord::index(x, y, z)] = s.at(x, y, z);
            }
        }
    }
    r.base_hash = base_hash(key, r.samples);
}

}  // namespace

Result<EditResult> EditStore::apply(BrushOp op, const DensitySource& source, JobSystem& jobs) {
    VEACH_ZONE("voxel: apply brush");
    if (auto valid = validate(op); !valid) {
        return std::unexpected(valid.error());
    }
    if (op.seq == 0) {
        op.seq = last_seq_ + 1;
    } else if (op.seq <= last_seq_) {
        return make_error(ErrorCode::voxel_brush_invalid,
                          std::format("brush op seq {} comes after seq {}", op.seq, last_seq_),
                          "Ops apply in sequence order per body (ADR 0054): give a larger seq, or 0 for the next.");
    }
    EditResult result;
    result.seq = op.seq;
    result.box = brush_box(op);
    // LOD0 chunks whose owned samples (32k .. 32k + 31) meet the box, in key order.
    std::vector<ChunkKey> keys;
    std::array<std::int64_t, 3> lo{}, hi{};
    for (std::size_t a = 0; a < 3; ++a) {
        lo[a] = floor_div(result.box.lo[a], chunk_cells);
        hi[a] = floor_div(result.box.hi[a], chunk_cells);
    }
    for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
        for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
                keys.push_back(
                    {0, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)});
            }
        }
    }
    // Records are created here, filled and edited on jobs (one chunk per job, indexed slots).
    std::vector<OverrideRecord*> slots(keys.size());
    std::vector<std::uint8_t> fresh(keys.size(), 0);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto [it, inserted] = records_.try_emplace(keys[i]);
        if (inserted) {
            it->second.key = keys[i];
            fresh[i] = 1;
        }
        slots[i] = &it->second;
    }
    std::vector<std::size_t> changed(keys.size(), 0);
    const Kernel kernel(op, encoding_);
    jobs.parallel_for("voxel: brush", keys.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            OverrideRecord& r = *slots[i];
            if (fresh[i] != 0) {
                generate_owned(source, r.key, r);
                r.mask.assign(OverrideRecord::owned_count, 0);
            }
            const auto o = chunk_origin(r.key);
            std::array<int, 3> l{}, h{};
            for (std::size_t a = 0; a < 3; ++a) {
                l[a] = static_cast<int>(std::max<std::int64_t>(0, result.box.lo[a] - o[a]));
                h[a] = static_cast<int>(std::min<std::int64_t>(chunk_cells - 1, result.box.hi[a] - o[a]));
            }
            std::size_t count = 0;
            for (int z = l[2]; z <= h[2]; ++z) {
                for (int y = l[1]; y <= h[1]; ++y) {
                    for (int x = l[0]; x <= h[0]; ++x) {
                        const std::size_t k = OverrideRecord::index(x, y, z);
                        if (kernel.apply(o[0] + x, o[1] + y, o[2] + z, r.samples[k])) {
                            r.overridden += r.mask[k] == 0 ? 1 : 0;
                            r.mask[k] = 1;
                            ++count;
                        }
                    }
                }
            }
            changed[i] = count;
        }
    });
    for (std::size_t i = 0; i < keys.size(); ++i) {
        result.materialized += fresh[i];
        if (changed[i] != 0) {
            result.changed.push_back(keys[i]);
            result.samples_changed += changed[i];
        } else if (slots[i]->overridden == 0) {
            records_.erase(keys[i]);  // touched, nothing written: no record
        }
    }
    journal_.push_back(op);
    last_seq_ = op.seq;
    return result;
}

const OverrideRecord* EditStore::find(const ChunkKey& key) const {
    const auto it = records_.find(key);
    return it == records_.end() ? nullptr : &it->second;
}

std::vector<BrushOp> EditStore::fold_journal() {
    return std::exchange(journal_, {});
}

std::uint64_t EditStore::hash() const {
    std::uint64_t h = hash_id("veach:voxel.edits");
    for (const auto& [key, record] : records_) {
        h = absorb(h, record_hash(record));
    }
    return h;
}

std::size_t EditStore::override_samples() const {
    std::size_t n = 0;
    for (const auto& [key, record] : records_) {
        n += record.overridden;
    }
    return n;
}

bool EditStore::touches(const LatticeBox& box) const {
    if (records_.empty()) {
        return false;
    }
    for (const auto& [key, record] : records_) {
        const auto o = chunk_origin(key);
        const LatticeBox owned{o, {o[0] + chunk_cells - 1, o[1] + chunk_cells - 1, o[2] + chunk_cells - 1}};
        if (owned.intersects(box)) {
            return true;
        }
    }
    return false;
}

namespace {

/// The overrides of `r` on a box of lattice points first + step * i (i < count per axis), stored at
/// data[sum i_a * stride_a]. Returns the samples written.
std::size_t overlay_box(const OverrideRecord& r, const std::array<std::int64_t, 3>& first, std::int64_t step,
                        const std::array<int, 3>& count, const std::array<std::size_t, 3>& stride, Sample* data) {
    const auto o = chunk_origin(r.key);
    std::array<int, 3> lo{}, hi{};
    for (std::size_t a = 0; a < 3; ++a) {
        // Indices i with o <= first + step i <= o + 31.
        lo[a] = static_cast<int>(std::max<std::int64_t>(0, ceil_div(o[a] - first[a], step)));
        hi[a] =
            static_cast<int>(std::min<std::int64_t>(count[a] - 1, floor_div(o[a] + chunk_cells - 1 - first[a], step)));
        if (lo[a] > hi[a]) {
            return 0;
        }
    }
    std::size_t written = 0;
    for (int z = lo[2]; z <= hi[2]; ++z) {
        const auto lz = static_cast<int>(first[2] + step * z - o[2]);
        for (int y = lo[1]; y <= hi[1]; ++y) {
            const auto ly = static_cast<int>(first[1] + step * y - o[1]);
            for (int x = lo[0]; x <= hi[0]; ++x) {
                const auto lx = static_cast<int>(first[0] + step * x - o[0]);
                const std::size_t k = OverrideRecord::index(lx, ly, lz);
                if (r.mask[k] != 0) {
                    data[static_cast<std::size_t>(x) * stride[0] + static_cast<std::size_t>(y) * stride[1] +
                         static_cast<std::size_t>(z) * stride[2]] = r.samples[k];
                    ++written;
                }
            }
        }
    }
    return written;
}

}  // namespace

void EditStore::overlay(ChunkSamples& s) const {
    if (records_.empty()) {
        return;
    }
    const LatticeBox box = sample_box(s.key);
    std::vector<const OverrideRecord*> hits;
    std::array<std::int64_t, 3> lo{}, hi{};
    std::int64_t volume = 1;
    for (std::size_t a = 0; a < 3; ++a) {
        lo[a] = floor_div(box.lo[a], chunk_cells);
        hi[a] = floor_div(box.hi[a], chunk_cells);
        volume *= hi[a] - lo[a] + 1;
    }
    if (volume <= static_cast<std::int64_t>(records_.size())) {
        for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
            for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
                for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
                    if (const OverrideRecord* r = find({0, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                                        static_cast<std::int32_t>(z)})) {
                        hits.push_back(r);
                    }
                }
            }
        }
    } else {
        for (const auto& [key, record] : records_) {
            if (key.x >= lo[0] && key.x <= hi[0] && key.y >= lo[1] && key.y <= hi[1] && key.z >= lo[2] &&
                key.z <= hi[2]) {
                hits.push_back(&record);
            }
        }
    }
    if (hits.empty()) {
        return;
    }
    constexpr int n = ChunkSamples::grid_n;
    constexpr int fn = ChunkSamples::face_n;
    const auto o = chunk_origin(s.key);
    const std::int64_t step = cell_units(s.key.level);
    std::size_t written = 0;
    for (const OverrideRecord* r : hits) {
        written += overlay_box(*r, {o[0] - step, o[1] - step, o[2] - step}, step, {n, n, n},
                               {1, std::size_t{n}, std::size_t{n} * n}, s.grid.data());
        for (int f = 0; f < 6; ++f) {
            auto& face = s.faces[static_cast<std::size_t>(f)];
            if (face.empty()) {
                continue;
            }
            const auto a = static_cast<std::size_t>(f / 2);
            const auto b = (a + 1) % 3, c = (a + 2) % 3;
            const std::int64_t h = step / 2;
            std::array<std::int64_t, 3> first{};
            std::array<int, 3> count{};
            std::array<std::size_t, 3> stride{};
            first[a] = o[a] + ((f & 1) ? chunk_units(s.key.level) : 0) - h;
            count[a] = 3;
            stride[a] = std::size_t{fn} * fn;
            first[b] = o[b] - h;
            count[b] = fn;
            stride[b] = 1;
            first[c] = o[c] - h;
            count[c] = fn;
            stride[c] = fn;
            written += overlay_box(*r, first, h, count, stride, face.data());
        }
    }
    if (written == 0) {
        return;
    }
    bool neg = false, pos = false;
    for (const Sample& v : s.grid) {
        (v.distance < 0 ? neg : pos) = true;
    }
    for (int f = 0; f < 6; ++f) {
        const auto& face = s.faces[static_cast<std::size_t>(f)];
        if (face.empty()) {
            continue;
        }
        for (int v = -1; v <= 2 * chunk_cells + 1; ++v) {
            for (int u = -1; u <= 2 * chunk_cells + 1; ++u) {
                (s.face_at(f, u, v, 0).distance < 0 ? neg : pos) = true;
            }
        }
    }
    s.has_surface = neg && pos;
}

bool EditStore::restore(const OverrideRecord& record, const DensitySource& source) {
    OverrideRecord r;
    r.key = record.key;
    generate_owned(source, r.key, r);
    const bool matched = r.base_hash == record.base_hash;
    if (!matched) {
        log::error("voxel", Error{ErrorCode::voxel_base_mismatch,
                                  std::format("chunk ({}, {}, {}): the regenerated base hashes to {:#018x}, the "
                                              "record was made on {:#018x}",
                                              r.key.x, r.key.y, r.key.z, r.base_hash, record.base_hash),
                                  "Pin the generator version the body was edited with; the overrides still apply."});
    }
    r.base_hash = record.base_hash;
    r.mask = record.mask;
    r.mask.resize(OverrideRecord::owned_count, 0);
    r.overridden = 0;
    for (std::size_t k = 0; k < r.mask.size(); ++k) {
        if (r.mask[k] != 0) {
            r.samples[k] = record.samples[k];
            ++r.overridden;
        }
    }
    records_[r.key] = std::move(r);
    return matched;
}

}  // namespace veach::voxel
