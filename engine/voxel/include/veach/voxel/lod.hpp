#pragma once

// Octree LOD selection for streaming (docs/adr/0053-terrain-density-field.md, docs/specs/voxel-terrain/spec.md),
// promoted from spike SP-09's Selector.
//
// The octree covers a box of root chunks on the body-local lattice. A node at level l > 0 is split when a viewer is
// closer to its box than lambda x (the child chunk edge); with several viewers the nearest counts. Hysteresis: a
// node that was split last time stays split until every viewer is beyond lambda x (1 + hysteresis) child edges, so a
// viewer hovering at a threshold does not flip it every frame. Nodes the classifier proves empty or solid are pruned
// (no chunk, nothing sampled). The result is restricted: leaves that touch (faces, edges or corners) differ by at
// most one level. Each leaf carries the faces whose neighbor is one level finer, where Transvoxel's transition cells
// go (docs/adr/0074-amendments-from-sp-08.md rule 2: the chunk is remeshed when that mask changes).
//
// Not canonical: the selection depends on viewer positions in floating point. What it selects is meshed canonically.

#include <veach/voxel/density.hpp>
#include <veach/voxel/voxel.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace veach::voxel {

struct ChunkKeyHash {
    std::size_t operator()(const ChunkKey& k) const;
};

/// A chunk to draw and the faces whose neighbor is finer: one mesh (ADR 0074 rule 2).
struct ChunkSlot {
    ChunkKey key;
    std::uint8_t finer = 0;
    auto operator<=>(const ChunkSlot&) const = default;
};
struct ChunkSlotHash {
    std::size_t operator()(const ChunkSlot& s) const { return ChunkKeyHash{}(s.key) ^ (std::size_t{s.finer} << 1); }
};

struct OctreeBounds {
    int root_level = 5;
    std::array<std::int32_t, 3> root_min{-1, -1, -1};  ///< root keys per axis, root_min..root_max inclusive
    std::array<std::int32_t, 3> root_max{0, 0, 0};
    /// Smallest whole octree that holds a ball of `radius_m` around the lattice origin: 2 x 2 x 2 roots.
    [[nodiscard]] static OctreeBounds around_origin(double radius_m);
};

/// Classifies chunks for the selector, a batch at a time (it may use jobs). out[i] is the class of keys[i]'s
/// sample box.
using ChunkClassifier = std::function<void(std::span<const ChunkKey> keys, std::span<BoxClass> out)>;

class LodSelector {
public:
    explicit LodSelector(OctreeBounds bounds) : bounds_(bounds) {}

    /// The restricted octree around `viewers` (body-local meters), sorted by key. `lambda` is the effective ratio;
    /// `hysteresis` the merge margin (0.1: merge beyond 1.1 lambda). Classes are cached per key.
    [[nodiscard]] std::vector<ChunkSlot> select(std::span<const std::array<double, 3>> viewers, double lambda,
                                                double hysteresis, const ChunkClassifier& classify);

    /// Forgets the cached classes of every chunk whose sample box meets `box` (after an edit there).
    void invalidate(const LatticeBox& box);
    /// Forgets every cached class.
    void clear_classes() { classes_.clear(); }
    [[nodiscard]] std::size_t cached_classes() const { return classes_.size(); }
    [[nodiscard]] const OctreeBounds& bounds() const { return bounds_; }
    /// Chunks classified so far by the classifier (not from the cache).
    [[nodiscard]] std::uint64_t classified() const { return classified_; }

private:
    void classify_missing(std::span<const ChunkKey> keys, const ChunkClassifier& classify);
    [[nodiscard]] bool touches_split_finer(const ChunkKey& k) const;

    OctreeBounds bounds_;
    std::unordered_map<ChunkKey, BoxClass, ChunkKeyHash> classes_;
    std::unordered_set<ChunkKey, ChunkKeyHash> interior_;      ///< split nodes of this selection
    std::unordered_set<ChunkKey, ChunkKeyHash> was_interior_;  ///< and of the previous one
    std::uint64_t classified_ = 0;
};

/// The child `i` (0..7, bit a for axis a) of a node at level > 0.
[[nodiscard]] constexpr ChunkKey child_of(const ChunkKey& k, int i) {
    return {static_cast<std::uint8_t>(k.level - 1), k.x * 2 + (i & 1), k.y * 2 + ((i >> 1) & 1),
            k.z * 2 + ((i >> 2) & 1)};
}

}  // namespace veach::voxel
