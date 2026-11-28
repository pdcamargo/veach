#pragma once

// veach::voxel, spike SP-08 (VCH-084), throwaway: density chunks, the IsoMesher interface and two CPU meshers
// (docs/research/spikes/sp-08-mesher.md). The real area (ADR 0065) replaces this; ADR 0053 fixes the constants below.
//
// Units. Positions are integers on the LOD0 lattice: one unit is one LOD0 cell, 0.5 m. A chunk at level l has 32³ cells
// of 2^l units and 33³ corner samples (plus a one-sample apron for gradients). Vertex positions are fixed point:
// LOD0 units × 2^16, so a vertex computed by two chunks from the same samples is the same integer in both, and mesh
// hashes are integer-only (canonical, ADR 0043).

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace veach::voxel {

inline constexpr int chunk_cells = 32;                    ///< cells per chunk edge, every level
inline constexpr double lod0_cell_meters = 0.5;           ///< one LOD0 lattice unit
inline constexpr double distance_quantum = 1.0 / 1024.0;  ///< meters per int16 sample step (±32 m band)
inline constexpr int fixed_bits = 16;                     ///< vertex positions: LOD0 units × 2^16
inline constexpr std::int64_t fixed_one = std::int64_t{1} << fixed_bits;

/// One chunk of a body-local octree (ADR 0053's key without the body address).
struct ChunkKey {
    std::uint8_t level = 0;
    std::int32_t x = 0, y = 0, z = 0;
    auto operator<=>(const ChunkKey&) const = default;
};

/// Cell edge at `level`, in LOD0 units.
constexpr std::int64_t cell_units(int level) {
    return std::int64_t{1} << level;
}
/// Chunk edge at `level`, in LOD0 units.
constexpr std::int64_t chunk_units(int level) {
    return chunk_cells * cell_units(level);
}
/// The chunk's minimum corner, in LOD0 units.
constexpr std::array<std::int64_t, 3> chunk_origin(const ChunkKey& k) {
    const std::int64_t e = chunk_units(k.level);
    return {k.x * e, k.y * e, k.z * e};
}

/// Faces, as bits of a 6-bit mask: bit 2a is the minimum face on axis a, bit 2a + 1 the maximum one.
enum Face : std::uint8_t { neg_x = 0, pos_x = 1, neg_y = 2, pos_y = 3, neg_z = 4, pos_z = 5 };

/// How a Sample's int16 distance encodes meters. `linear` is ADR 0053's: 1/1024 m, clamped to ±32 m. `companded`
/// (spike SP-09) is the same within ±16 m and logarithmic beyond, with the same slope at the knee, reaching 32767 at
/// 10,000 km (about 0.1 % of the distance per step): coarse cells (up to 262 km at level 19) keep their distances, so
/// vertex placement and gradient normals stay right where the linear band is saturated. The sign is the same in both.
enum class DistanceEncoding : std::uint8_t { linear, companded };

/// The code of a signed distance in meters (round to nearest, ties away from zero; saturates at ±32767). Canonical.
std::int16_t encode_distance(double meters, DistanceEncoding encoding);
/// The distance a code stands for, meters. Canonical.
double decode_distance(std::int16_t code, DistanceEncoding encoding);

/// A density sample (ADR 0053): signed distance as an int16 code (DistanceEncoding), negative solid; a material id.
struct Sample {
    std::int16_t distance = 0;
    std::uint8_t material = 0;
};

/// The field a chunk samples: a pure function of the LOD0 lattice point, never of the chunk or level asking.
class DensityField {
public:
    virtual ~DensityField() = default;
    virtual Sample sample(std::int64_t x, std::int64_t y, std::int64_t z) const = 0;
};

/// Samples of one chunk. `grid` has (32 + 3)³ samples at the chunk's spacing, from -1 to 33 on each axis (the apron
/// gives central-difference gradients on the boundary). `faces[f]`, only for faces the mesher asked for, has 3 layers
/// (half-spacing steps -1, 0 and +1 along the face's axis, 0 on the plane) of (64 + 3)² samples at half the spacing,
/// from -1 to 65 along the face's other two axes (u along (axis + 1) % 3, v along (axis + 2) % 3): the
/// full-resolution face of Transvoxel's transition cells.
struct ChunkSamples {
    static constexpr int grid_n = chunk_cells + 3;
    static constexpr int face_n = 2 * chunk_cells + 3;
    ChunkKey key;
    std::vector<Sample> grid;
    std::array<std::vector<Sample>, 6> faces;
    std::uint8_t face_mask = 0;
    bool has_surface = false;  ///< any sign change in `grid` or a sampled face

    static constexpr std::size_t grid_index(int x, int y, int z) {
        return static_cast<std::size_t>((x + 1) + grid_n * ((y + 1) + grid_n * (z + 1)));
    }
    const Sample& at(int x, int y, int z) const { return grid[grid_index(x, y, z)]; }
    Sample& at(int x, int y, int z) { return grid[grid_index(x, y, z)]; }
    /// Face sample at in-plane (u, v) in -1..65 and layer -1, 0 or 1 (steps along the axis; 0 is the plane).
    const Sample& face_at(int face, int u, int v, int layer) const {
        return faces[static_cast<std::size_t>(face)]
                    [static_cast<std::size_t>((u + 1) + face_n * ((v + 1) + face_n * (layer + 1)))];
    }
};

/// Fills `out` for `key`, plus the full-resolution face samples of the faces in `face_mask`.
void sample_chunk(const DensityField& field, const ChunkKey& key, std::uint8_t face_mask, ChunkSamples& out);

/// A chunk's mesh. `fixed` is the canonical position (global, LOD0 units × 2^16); `position` the same point relative to
/// the chunk origin in meters, for rendering. Normals point out of the solid.
struct ChunkMesh {
    ChunkKey key;
    std::vector<std::array<std::int64_t, 3>> fixed;
    std::vector<std::array<float, 3>> position;
    std::vector<std::array<float, 3>> normal;
    std::vector<std::uint8_t> material;
    std::vector<std::uint32_t> indices;
    std::uint32_t transition_triangles = 0;  ///< Transvoxel transition cells
    std::uint32_t skirt_triangles = 0;       ///< surface nets skirts

    void clear();
    std::size_t triangle_count() const { return indices.size() / 3; }
};

/// What a chunk needs to know about its neighbors: faces whose neighbor is one level finer, and one level coarser;
/// `touching` adds the faces whose edges or corners touch a chunk of another level (diagonal neighbors).
struct ChunkNeighbors {
    std::uint8_t finer = 0;
    std::uint8_t coarser = 0;
    std::uint8_t touching = 0;
};

/// A CPU iso-surface extractor (ADR 0053). Not thread-safe: it owns scratch memory, so use one per worker.
class IsoMesher {
public:
    virtual ~IsoMesher() = default;
    virtual std::string_view name() const = 0;
    /// The faces whose full-resolution samples mesh() reads, for sample_chunk().
    virtual std::uint8_t face_samples_needed(ChunkNeighbors neighbors) const = 0;
    virtual void mesh(const ChunkSamples& samples, ChunkNeighbors neighbors, ChunkMesh& out) = 0;
};

struct TransvoxelOptions {
    bool transitions = true;  ///< false: plain marching cubes (the cracked control)
    /// Transition cell width as a fraction of a cell, in 1/256: regular vertices in the boundary cell layer are pushed
    /// inward to make room (Lengyel's shrink). 0 leaves the transition cells flat in the face plane.
    int width_256 = 128;
    /// Project the shrink on the tangent plane (Lengyel's 4.4.2); false moves vertices straight along the axes.
    bool project = true;
    /// How to read the samples' distances. Linear interpolates and differences the codes themselves (SP-08's
    /// committed hashes); companded decodes them to meters first.
    DistanceEncoding encoding = DistanceEncoding::linear;
};
std::unique_ptr<IsoMesher> make_transvoxel_mesher(TransvoxelOptions options = {});

struct SurfaceNetsOptions {
    bool skirts = true;  ///< skirts on faces whose neighbor has another level
    /// Skirt length in cells of the chunk's level, in 1/256.
    int skirt_256 = 256;
};
std::unique_ptr<IsoMesher> make_surface_nets_mesher(SurfaceNetsOptions options = {});

/// Integer-only hash of a mesh (key, fixed positions, normals quantized to 1/32767, materials, indices), canonical.
std::uint64_t mesh_hash(const ChunkMesh& mesh);
/// Folds chunk hashes, in the order given.
std::uint64_t combine_hashes(const std::vector<std::uint64_t>& hashes);

}  // namespace veach::voxel
