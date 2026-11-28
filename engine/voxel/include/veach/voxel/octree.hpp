#pragma once

// Spike SP-08 (VCH-084), throwaway: distance-based octree LOD selection, restricted to one level of difference between
// touching chunks, and each chunk's neighbor masks (ADR 0053).

#include <veach/voxel/voxel.hpp>

#include <vector>

namespace veach::voxel {

struct LodSettings {
    std::array<double, 3> focus{};  ///< LOD0 units (the camera)
    /// A node at level l > 0 is split when the focus is closer to its box than lambda × (the child chunk edge).
    double lambda = 4.0;
    int root_level = 5;
    std::array<std::int32_t, 3> root_min{-1, -1, -1};  ///< root keys per axis: root_min..root_max inclusive
    std::array<std::int32_t, 3> root_max{0, 0, 0};
};

struct ChunkLeaf {
    ChunkKey key;
    ChunkNeighbors neighbors;
};

/// Leaves of the octree, sorted by key, every touching pair (face, edge or corner) within one level of each other.
std::vector<ChunkLeaf> select_chunks(const LodSettings& settings);

}  // namespace veach::voxel
