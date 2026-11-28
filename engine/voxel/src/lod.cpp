// Octree LOD selection (lod.hpp), promoted from spike SP-09 (docs/research/spikes/sp-09-descent.md).

#include <veach/core/hash.hpp>
#include <veach/core/profile.hpp>
#include <veach/voxel/lod.hpp>

#include <algorithm>
#include <cmath>

namespace veach::voxel {

std::size_t ChunkKeyHash::operator()(const ChunkKey& k) const {
    std::uint64_t h = mix64(std::uint64_t{k.level} + hash_golden);
    h = mix64(h ^ static_cast<std::uint32_t>(k.x));
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.y)) << 21));
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(k.z)) << 42));
    return static_cast<std::size_t>(h);
}

OctreeBounds OctreeBounds::around_origin(double radius_m) {
    OctreeBounds b;
    b.root_level = 0;
    while (static_cast<double>(chunk_units(b.root_level)) * lod0_cell_meters < radius_m && b.root_level < 30) {
        ++b.root_level;
    }
    b.root_min = {-1, -1, -1};
    b.root_max = {0, 0, 0};
    return b;
}

namespace {

double distance_sq(const std::array<double, 3>& p, const ChunkKey& k) {
    const auto o = chunk_origin(k);
    const auto e = static_cast<double>(chunk_units(k.level));
    double d2 = 0.0;
    for (std::size_t a = 0; a < 3; ++a) {
        const auto lo = static_cast<double>(o[a]);
        const double hi = lo + e;
        const double d = p[a] < lo ? lo - p[a] : (p[a] > hi ? p[a] - hi : 0.0);
        d2 += d * d;
    }
    return d2;
}

}  // namespace

void LodSelector::classify_missing(std::span<const ChunkKey> keys, const ChunkClassifier& classify) {
    std::vector<ChunkKey> missing;
    for (const ChunkKey& k : keys) {
        if (!classes_.contains(k)) {
            missing.push_back(k);
        }
    }
    if (missing.empty()) {
        return;
    }
    std::vector<BoxClass> out(missing.size(), BoxClass::surface);
    classify(missing, out);
    for (std::size_t i = 0; i < missing.size(); ++i) {
        classes_.emplace(missing[i], out[i]);
    }
    classified_ += missing.size();
}

bool LodSelector::touches_split_finer(const ChunkKey& k) const {
    const auto level = static_cast<std::uint8_t>(k.level - 1);
    for (std::int32_t dz = -1; dz <= 2; ++dz) {
        for (std::int32_t dy = -1; dy <= 2; ++dy) {
            for (std::int32_t dx = -1; dx <= 2; ++dx) {
                if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1 && dz >= 0 && dz <= 1) {
                    continue;  // k's own children
                }
                if (interior_.contains({level, 2 * k.x + dx, 2 * k.y + dy, 2 * k.z + dz})) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<ChunkSlot> LodSelector::select(std::span<const std::array<double, 3>> viewers, double lambda,
                                           double hysteresis, const ChunkClassifier& classify) {
    VEACH_ZONE("voxel: select");
    interior_.clear();
    std::vector<std::array<double, 3>> focus;
    focus.reserve(viewers.size());
    for (const auto& v : viewers) {
        focus.push_back({v[0] / lod0_cell_meters, v[1] / lod0_cell_meters, v[2] / lod0_cell_meters});
    }
    const auto split = [&](const ChunkKey& k) {
        if (k.level == 0 || focus.empty()) {
            return false;
        }
        const double edges = was_interior_.contains(k) ? lambda * (1.0 + hysteresis) : lambda;
        const double reach = edges * static_cast<double>(chunk_units(k.level - 1));
        for (const auto& f : focus) {
            if (distance_sq(f, k) < reach * reach) {
                return true;
            }
        }
        return false;
    };
    const auto surface = [&](const ChunkKey& k) {
        return classes_.at(k) == BoxClass::surface;
    };

    std::vector<ChunkKey> frontier;
    for (std::int32_t z = bounds_.root_min[2]; z <= bounds_.root_max[2]; ++z) {
        for (std::int32_t y = bounds_.root_min[1]; y <= bounds_.root_max[1]; ++y) {
            for (std::int32_t x = bounds_.root_min[0]; x <= bounds_.root_max[0]; ++x) {
                frontier.push_back({static_cast<std::uint8_t>(bounds_.root_level), x, y, z});
            }
        }
    }
    classify_missing(frontier, classify);
    std::erase_if(frontier, [&](const ChunkKey& k) { return !surface(k); });

    // Distance refinement, one level at a time, so each level's new nodes are classified in one batch.
    std::vector<ChunkKey> leaves;
    while (!frontier.empty()) {
        std::vector<ChunkKey> children;
        for (const ChunkKey& k : frontier) {
            if (split(k)) {
                interior_.insert(k);
                for (int i = 0; i < 8; ++i) {
                    children.push_back(child_of(k, i));
                }
            } else {
                leaves.push_back(k);
            }
        }
        classify_missing(children, classify);
        std::erase_if(children, [&](const ChunkKey& k) { return !surface(k); });
        frontier = std::move(children);
    }

    // Restrict: a leaf touching a split node one level finer than itself touches leaves two levels finer. Splits are
    // decided per pass from the pass's starting state; splitting only ever adds, so the fixed point is unique.
    for (;;) {
        std::vector<ChunkKey> split_now;
        for (const ChunkKey& k : leaves) {
            if (k.level >= 2 && touches_split_finer(k)) {
                split_now.push_back(k);
            }
        }
        if (split_now.empty()) {
            break;
        }
        std::vector<ChunkKey> children;
        for (const ChunkKey& k : split_now) {
            interior_.insert(k);
            for (int i = 0; i < 8; ++i) {
                children.push_back(child_of(k, i));
            }
        }
        classify_missing(children, classify);
        std::erase_if(leaves, [&](const ChunkKey& k) { return interior_.contains(k); });
        for (const ChunkKey& c : children) {
            if (surface(c)) {
                leaves.push_back(c);
            }
        }
    }

    std::vector<ChunkSlot> out;
    out.reserve(leaves.size());
    for (const ChunkKey& k : leaves) {
        std::uint8_t finer = 0;
        if (k.level > 0) {
            for (int f = 0; f < 6; ++f) {
                ChunkKey n = k;
                const int step = (f & 1) ? 1 : -1;
                (f / 2 == 0 ? n.x : (f / 2 == 1 ? n.y : n.z)) += step;
                if (interior_.contains(n)) {
                    finer = static_cast<std::uint8_t>(finer | (1U << f));
                }
            }
        }
        out.push_back({k, finer});
    }
    std::ranges::sort(out);
    was_interior_ = interior_;
    return out;
}

void LodSelector::invalidate(const LatticeBox& box) {
    std::erase_if(classes_, [&](const auto& entry) { return sample_box(entry.first).intersects(box); });
}

}  // namespace veach::voxel
