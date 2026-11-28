// Spike SP-12 (VCH-088), throwaway: a patch of LOD0 terrain on an Earth-sized planet, dug with brush ops and meshed
// on veach::JobSystem (docs/research/spikes/sp-12-dig-collide.md). Colliders are engine/physics's (VCH-113).
#pragma once

#include <veach/core/jobs.hpp>
#include <veach/physics/physics.hpp>
#include <veach/voxel/edit.hpp>
#include <veach/voxel/voxel.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace veach::sp12 {

using voxel::BrushOp;
using voxel::ChunkKey;
using voxel::ChunkMesh;
using voxel::ChunkSamples;

/// A rocky planet of Earth's radius in body-local meters (body-fixed frame, center at the origin): three octaves of
/// veach::hash value noise on the radius (±7.85 m), regolith over rock 3 m down. Canonical: + − × ÷ √ and floor only,
/// randomness from veach::hash, a pure function of the lattice point (ADR 0074).
struct PlanetField final : voxel::DensityField {
    static constexpr double radius = 6'371'000.0;
    static constexpr double relief = 7.85;  ///< bound on |noise|, meters
    std::uint64_t seed = 0x5112'0001;

    double distance(double px, double py, double pz) const;
    voxel::Sample sample(std::int64_t x, std::int64_t y, std::int64_t z) const override;
    /// False when the chunk's sample grid (apron included) provably holds no surface: all of it farther from the
    /// mean radius than the relief, plus a margin.
    bool may_have_surface(const ChunkKey& key) const;
};

/// The dig site: on the surface along the 3-4-5 direction (0.6, 0.8, 0) from the center, 6,371 km out, so two axes
/// are large (the SP-11 placement). Integer body-local millimeters, found once by bisection on the field and
/// committed as data (an op log's centers are data, not computations).
inline constexpr std::array<std::int64_t, 3> dig_site_mm{3'822'603'093, 5'096'792'457, 0};
/// Radial up at the site, and two tangents (exact decimal fractions).
inline constexpr std::array<double, 3> site_up{0.6, 0.8, 0.0};

/// `site` moved by (east, north, up) millimeters: east is (0.8, −0.6, 0), north (0, 0, 1), up (0.6, 0.8, 0).
/// Integer: the offsets must be multiples of 5 mm.
std::array<std::int64_t, 3> site_offset_mm(std::int64_t east, std::int64_t north, std::int64_t up);

/// A dig site anywhere on the planet (the seams sweep, docs/research/spikes/seams-sweep.md): a surface point in integer
/// body-local millimeters and its tangent frame (unit up, east, north; north = up × east).
struct Site {
    std::array<std::int64_t, 3> center_mm{};
    std::array<double, 3> up{}, east{}, north{};
};
/// SP-12's site, along (0.6, 0.8, 0).
inline constexpr Site default_site{dig_site_mm, site_up, {0.8, -0.6, 0.0}, {0.0, 0.0, 1.0}};
/// `site` moved by (east, north, up) millimeters, rounded to the millimeter (+ − × and nearbyint: canonical). For
/// default_site and offsets that are multiples of 5 mm it equals site_offset_mm(east, north, up).
std::array<std::int64_t, 3> site_offset_mm(const Site& site, std::int64_t east, std::int64_t north, std::int64_t up);

/// The LOD0 chunk holding a body-local point in millimeters.
ChunkKey chunk_of_mm(const std::array<std::int64_t, 3>& p);

/// A chunk's mesh as a physics collider mesh: its canonical fixed-point vertices and indices, no adjacency ring. The
/// stand-in for the voxel bridge of VCH-114 (docs/specs/physics/spec.md, "Terrain colliders").
physics::ColliderMesh collider_mesh(const ChunkMesh& mesh);
/// The physics key of a voxel chunk key (the same fields in the same order).
physics::ChunkKey physics_key(const ChunkKey& key);

/// What a chunk costs, in ms of one worker, last time it was produced.
struct ChunkTimes {
    double sample = 0.0;
    double edit = 0.0;
    double mesh = 0.0;
};

struct Chunk {
    ChunkSamples samples;      ///< empty grid until sampled
    voxel::OverrideMask mask;  ///< empty until an op writes the chunk
    ChunkMesh mesh;
    ChunkTimes times;
    bool sampled = false;
};

/// A cube of (2 half + 1)³ LOD0 chunks around `center`, all held in memory (ADR 0053 keeps LOD0 samples around
/// players). Chunks the bounds prove uniform are not sampled until an op touches them.
class Terrain {
public:
    Terrain(const PlanetField& field, ChunkKey center, int half);

    /// Samples every chunk that may have surface (every chunk when `eager`), then meshes them, on `jobs`.
    void load(JobSystem& jobs, bool eager = false);
    /// Applies `op` to every chunk it touches (each one independently, in parallel on `jobs`, in reverse key order
    /// when `reverse`), sampling chunks not sampled yet, and remeshes the changed ones. Returns the changed chunks in
    /// key order. Touched chunks outside the region are skipped and counted in outside_count().
    std::vector<ChunkKey> apply(const BrushOp& op, JobSystem& jobs, bool reverse = false);
    std::size_t outside_count() const { return outside_; }
    /// Chunks the bounds had proven uniform that an op touched, so apply() sampled them first.
    std::size_t materialized_count() const { return materialized_; }

    const std::vector<Chunk>& chunks() const { return chunks_; }
    const Chunk* find(const ChunkKey& key) const;
    ChunkKey center() const { return center_; }

    /// Folded samples_hash of every sampled chunk, in key order.
    std::uint64_t samples_hash() const;
    /// Folded mesh_hash of every sampled chunk, in key order (lattice-coordinate positions, ADR 0074).
    std::uint64_t mesh_hash() const;
    std::size_t sampled_count() const;
    std::size_t meshed_count() const;
    /// Overridden lattice points, each counted once (in the chunk that owns it: grid indices 0..31 on each axis):
    /// the samples ADR 0054's override records hold at rest.
    std::size_t override_samples() const;
    /// Pairs of neighboring sampled chunks whose shared samples (apron included) differ. Must be 0.
    std::size_t shared_sample_mismatches() const;

private:
    void produce(std::size_t index, bool sample, voxel::IsoMesher& mesher);
    voxel::IsoMesher& mesher(JobSystem& jobs);

    const PlanetField& field_;
    ChunkKey center_;
    std::vector<Chunk> chunks_;  ///< sorted by key
    std::map<ChunkKey, std::size_t> index_;
    std::vector<std::unique_ptr<voxel::IsoMesher>> meshers_;  ///< one per job thread
    std::size_t outside_ = 0;
    std::size_t materialized_ = 0;
};

}  // namespace veach::sp12
