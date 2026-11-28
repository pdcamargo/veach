// veach: canonical
// Spike SP-08 (VCH-084), throwaway: octree LOD selection (docs/research/spikes/sp-08-mesher.md).

#include <veach/voxel/octree.hpp>

#include <algorithm>
#include <map>

namespace veach::voxel {
namespace {

struct Box {
    std::array<std::int64_t, 3> lo, hi;
};

Box box_of(const ChunkKey& k) {
    const auto o = chunk_origin(k);
    const std::int64_t e = chunk_units(k.level);
    return {o, {o[0] + e, o[1] + e, o[2] + e}};
}

double distance_sq(const std::array<double, 3>& p, const Box& b) {
    double d2 = 0.0;
    for (int a = 0; a < 3; ++a) {
        const auto i = static_cast<std::size_t>(a);
        const double lo = static_cast<double>(b.lo[i]);
        const double hi = static_cast<double>(b.hi[i]);
        const double d = p[i] < lo ? lo - p[i] : (p[i] > hi ? p[i] - hi : 0.0);
        d2 += d * d;
    }
    return d2;
}

bool touches(const Box& a, const Box& b) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (a.lo[i] > b.hi[i] || b.lo[i] > a.hi[i]) {
            return false;
        }
    }
    return true;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

ChunkKey child(const ChunkKey& k, int i) {
    return ChunkKey{static_cast<std::uint8_t>(k.level - 1), k.x * 2 + (i & 1), k.y * 2 + ((i >> 1) & 1),
                    k.z * 2 + ((i >> 2) & 1)};
}

void refine(const LodSettings& s, const ChunkKey& k, std::vector<ChunkKey>& leaves) {
    if (k.level > 0) {
        const double reach = s.lambda * static_cast<double>(chunk_units(k.level - 1));
        if (distance_sq(s.focus, box_of(k)) < reach * reach) {
            for (int i = 0; i < 8; ++i) {
                refine(s, child(k, i), leaves);
            }
            return;
        }
    }
    leaves.push_back(k);
}

}  // namespace

std::vector<ChunkLeaf> select_chunks(const LodSettings& settings) {
    std::vector<ChunkKey> leaves;
    for (std::int32_t z = settings.root_min[2]; z <= settings.root_max[2]; ++z) {
        for (std::int32_t y = settings.root_min[1]; y <= settings.root_max[1]; ++y) {
            for (std::int32_t x = settings.root_min[0]; x <= settings.root_max[0]; ++x) {
                refine(settings, ChunkKey{static_cast<std::uint8_t>(settings.root_level), x, y, z}, leaves);
            }
        }
    }

    // Restrict: split any leaf that touches a leaf two or more levels finer, until none does.
    for (bool changed = true; changed;) {
        changed = false;
        std::sort(leaves.begin(), leaves.end());
        std::vector<ChunkKey> next;
        next.reserve(leaves.size());
        for (const ChunkKey& k : leaves) {
            bool split = false;
            if (k.level >= 2) {
                const Box b = box_of(k);
                for (const ChunkKey& other : leaves) {
                    if (other.level + 2 <= k.level && touches(b, box_of(other))) {
                        split = true;
                        break;
                    }
                }
            }
            if (split) {
                for (int i = 0; i < 8; ++i) {
                    next.push_back(child(k, i));
                }
                changed = true;
            } else {
                next.push_back(k);
            }
        }
        leaves = std::move(next);
    }
    std::sort(leaves.begin(), leaves.end());

    std::map<ChunkKey, int> by_key;
    for (const ChunkKey& k : leaves) {
        by_key.emplace(k, k.level);
    }
    const auto level_at = [&](const std::array<std::int64_t, 3>& p) -> int {
        for (int l = 0; l <= settings.root_level; ++l) {
            const std::int64_t e = chunk_units(l);
            const ChunkKey k{static_cast<std::uint8_t>(l), static_cast<std::int32_t>(floor_div(p[0], e)),
                             static_cast<std::int32_t>(floor_div(p[1], e)),
                             static_cast<std::int32_t>(floor_div(p[2], e))};
            if (by_key.contains(k)) {
                return l;
            }
        }
        return -1;
    };

    std::vector<ChunkLeaf> out;
    out.reserve(leaves.size());
    for (const ChunkKey& k : leaves) {
        ChunkLeaf leaf{k, {}};
        const Box b = box_of(k);
        const std::int64_t quarter = chunk_units(k.level) / 4;
        // One probe per direction (face, edge, corner): the LOD0 cell just outside, a quarter along the free axes.
        // In a restricted octree one probe sees the whole neighbor region's level.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const std::array<int, 3> d{dx, dy, dz};
                    if (dx == 0 && dy == 0 && dz == 0) {
                        continue;
                    }
                    std::array<std::int64_t, 3> probe{};
                    int axes = 0;
                    std::uint8_t faces = 0;
                    for (std::size_t a = 0; a < 3; ++a) {
                        probe[a] = d[a] < 0 ? b.lo[a] - 1 : (d[a] > 0 ? b.hi[a] : b.lo[a] + quarter);
                        if (d[a] != 0) {
                            ++axes;
                            faces = static_cast<std::uint8_t>(faces | (1u << (2 * a + (d[a] > 0 ? 1 : 0))));
                        }
                    }
                    const int l = level_at(probe);
                    if (l < 0 || l == k.level) {
                        continue;
                    }
                    leaf.neighbors.touching = static_cast<std::uint8_t>(leaf.neighbors.touching | faces);
                    if (axes == 1) {
                        if (l < k.level) {
                            leaf.neighbors.finer = static_cast<std::uint8_t>(leaf.neighbors.finer | faces);
                        } else {
                            leaf.neighbors.coarser = static_cast<std::uint8_t>(leaf.neighbors.coarser | faces);
                        }
                    }
                }
            }
        }
        out.push_back(leaf);
    }
    return out;
}

}  // namespace veach::voxel
