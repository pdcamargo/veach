// veach: canonical
// Spike SP-12 (VCH-088): brush operations applied to LOD0 samples in integer arithmetic (ADR 0054,
// docs/research/spikes/sp-12-dig-collide.md).

#include <veach/core/hash.hpp>
#include <veach/voxel/edit.hpp>

#include <algorithm>

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
    const std::int64_t reach = reach_mm(op);
    const std::int64_t reach2 = reach * reach;
    const std::int64_t radius_1024 = std::int64_t{op.radius_mm} * 1024;
    std::size_t changed = 0;
    for (int z = lo[2]; z <= hi[2]; ++z) {
        const std::int64_t dz = (o[2] + z) * lod0_cell_mm - op.center_mm[2];
        for (int y = lo[1]; y <= hi[1]; ++y) {
            const std::int64_t dy = (o[1] + y) * lod0_cell_mm - op.center_mm[1];
            for (int x = lo[0]; x <= hi[0]; ++x) {
                const std::int64_t dx = (o[0] + x) * lod0_cell_mm - op.center_mm[0];
                const std::int64_t d2 = dx * dx + dy * dy + dz * dz;  // mm², at most 3 · 17 m² · 10⁶
                if (d2 > reach2) {
                    continue;
                }
                // Distance to the brush surface in 1/1024 m, positive outside: 1024 · |p − c| is
                // isqrt(d2 · 1024²) (floor), then (· − 1024 r) / 1000 mm per m, rounded.
                const auto scaled = static_cast<std::int64_t>(isqrt(static_cast<std::uint64_t>(d2) << 20));
                const std::int64_t brush = round_div(scaled - radius_1024, 1000);
                Sample& sample = s.at(x, y, z);
                const std::int64_t old = sample.distance;
                std::int64_t value = old;
                std::uint8_t material = sample.material;
                if (op.kind == BrushKind::subtract) {
                    value = std::max(old, -brush);
                } else if (brush < old) {
                    value = brush;
                    material = op.material;
                }
                value = std::clamp<std::int64_t>(value, -32767, 32767);
                if (value != old || material != sample.material) {
                    sample.distance = static_cast<std::int16_t>(value);
                    sample.material = material;
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
        const std::uint64_t word = static_cast<std::uint16_t>(v.distance) | (std::uint64_t{v.material} << 16);
        h = mix64(h ^ mix64(word + hash_golden));
    }
    return h;
}

}  // namespace veach::voxel
