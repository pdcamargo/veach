#pragma once

// A streamed terrain body (docs/specs/voxel-terrain/spec.md, guide: docs/guides/voxel-terrain.md; ADR 0053, 0054,
// 0074, 0077, 0079, 0083). Promoted from spike SP-09 (octree, LOD, streaming, budget, adaptive lambda, weld check)
// and SP-12 (edits, LOD0 meshes for colliders).
//
// A TerrainBody is one signed density field on a body-local integer lattice (LOD0 cells of 0.5 m), in the body's own
// reference frame: a planet's body-fixed frame, an asteroid's, or a flat world's. Each update():
//
//   1. collects the chunk jobs launched by the previous update (they ran across the frame on veach::JobSystem),
//   2. selects the restricted octree around the viewers (LodSelector) with the effective lambda,
//   3. launches up to the budget's chunk jobs for what the selection lacks, coarse levels first: sample from the
//      DensitySource, overlay the edits, mesh (Transvoxel), hash, and run the optional payload hook,
//   4. switches the drawn set to the selection only when every chunk of it is meshed: never a hole. While the drawn
//      set lags by more than the budget's patience, the effective lambda drops (coarser, fewer chunks) and recovers
//      once it keeps up (ADR 0079 rule 3: never stall, never slow the simulation).
//
// What leaves: drawn() (what to draw, every record immutable and shared), ready() and evicted() (this update's
// changes, for a renderer's upload and release), and ColliderUpdates (LOD0 meshes with their adjacency ring, ADR
// 0083) for a ColliderSink, which engine/physics implements. The body never links render or physics (ADR 0065).
//
// Canonical: every chunk's samples, mesh and hash, the edits and the collider meshes are the same on every platform
// and for any number of workers. Which chunks are selected depends on viewer positions and the budget, not on the
// worker count when updates are synchronous.

#include <veach/core/error.hpp>
#include <veach/voxel/density.hpp>
#include <veach/voxel/edit.hpp>
#include <veach/voxel/lod.hpp>
#include <veach/voxel/voxel.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace veach {
class JobSystem;
class TaskGroup;
}  // namespace veach

namespace veach::voxel {

/// How much streaming work a machine does per frame (ADR 0053, 0079: set from chunks/s measured per tier).
struct TerrainStreamingBudget {
    std::uint32_t jobs_per_frame = 32;  ///< chunk jobs launched per update
    double min_lambda = 2.0;            ///< the effective lambda never drops below this
    std::uint32_t patience = 15;        ///< updates the drawn set may lag before lambda drops
    double lower = 0.97;                ///< lambda *= lower per update past the patience
    double recover = 1.005;             ///< lambda *= recover per caught-up update, up to TerrainSettings::lambda

    /// A budget for a measured rate: chunks/s this machine meshes, at `frames_per_second`.
    [[nodiscard]] static TerrainStreamingBudget from_rate(double chunks_per_second, double frames_per_second = 60.0);
};

/// One meshed chunk, immutable once published. Shared: the drawn set, ready() and a renderer may all hold it.
struct ChunkRecord {
    ChunkSlot slot;
    ChunkMesh mesh;  ///< lattice positions (canonical), chunk-relative float positions, normals, materials, indices
    std::uint64_t mesh_hash = 0;
    std::uint64_t serial = 0;             ///< increasing per body: a replaced chunk gets a new serial
    std::shared_ptr<const void> payload;  ///< from TerrainSettings::payload, made on the chunk's job
};
using ChunkRef = std::shared_ptr<const ChunkRecord>;

/// Run on the chunk's job after meshing, for chunks with triangles: a renderer builds its GPU-ready mesh here, off
/// the main thread. Must be thread-safe and must not wait on jobs.
using ChunkPayload = std::function<std::shared_ptr<const void>(const ChunkRecord& record)>;

struct TerrainSettings {
    OctreeBounds octree;
    double lambda = 4.0;      ///< ADR 0053's LOD ratio (a quality setting: 4 on M1-class, 6 on M3 Pro-class)
    double hysteresis = 0.1;  ///< merge beyond lambda * (1 + hysteresis) child edges
    TerrainStreamingBudget budget;
    /// Jobs run across frames: update() launches them and the next update() collects them. False: update() waits
    /// for its own jobs (tests, benchmarks, servers that tick terrain directly).
    bool asynchronous = true;
    TransvoxelOptions mesher;  ///< its encoding is replaced by the source's
    ChunkPayload payload;
    /// Colliders (ADR 0053, 0077, 0083): LOD0 chunks within reach of a probe's position over the lookahead, installed
    /// delay ticks after the tick that asked.
    double collider_reach_m = 32.0;
    double collider_lookahead_s = 0.25;
    std::uint32_t collider_delay_ticks = 2;
};

/// One update's numbers (not canonical: times are wall clock).
struct StreamStats {
    std::uint64_t update = 0;
    std::uint32_t target = 0;      ///< chunks the selection asks for
    std::uint32_t drawn = 0;       ///< chunks drawn (the last complete selection)
    std::uint32_t cached = 0;      ///< meshed chunks held
    std::uint32_t requests = 0;    ///< selection entries new since the last update
    std::uint32_t remeshes = 0;    ///< of those, chunks already selected with other finer faces
    std::uint32_t launched = 0;    ///< chunk jobs launched
    std::uint32_t completed = 0;   ///< chunk jobs collected
    std::uint32_t empty = 0;       ///< of those, no triangles
    std::uint32_t backlog = 0;     ///< selection entries still missing after this update's launches
    std::uint32_t stale = 0;       ///< updates since the drawn set last matched the selection
    std::uint32_t classified = 0;  ///< boxes classified (not cached) by this update's selection
    double lambda = 0.0;           ///< the effective lambda used
    double select_ms = 0.0;
    double update_ms = 0.0;  ///< on the calling thread, waiting for last update's jobs included
    double sample_ms = 0.0;  ///< summed over collected jobs (CPU time)
    double mesh_ms = 0.0;
};

/// A dynamic body that needs terrain colliders around it, body-local.
struct ColliderProbe {
    std::array<double, 3> position_m{};
    std::array<double, 3> velocity_m_s{};
};

/// A LOD0 chunk's collider mesh (ADR 0083): the chunk's own triangles, then its neighbors' border triangles as
/// adjacency only (they inform internal-edge handling and never produce contacts). Vertices are welded by lattice
/// position; a physics scene whose origin is a LOD0 grid point converts them exactly (ADR 0077 rule 6).
struct ColliderMesh {
    ChunkKey key;
    std::uint64_t hash = 0;                              ///< of the own mesh and the ring (canonical)
    std::vector<std::array<std::int64_t, 3>> positions;  ///< body-local, LOD0 units x 2^16 (voxel.hpp fixed point)
    std::vector<std::uint32_t> indices;
    std::uint32_t own_triangles = 0;  ///< triangles [0, own_triangles) collide; the rest are the ring
};

/// Colliders to change, installed together before step `install_tick`, in key order (ADR 0077 rule 4).
struct ColliderUpdate {
    std::uint64_t tick = 0;  ///< the tick that asked
    std::uint64_t install_tick = 0;
    std::vector<ColliderMesh> install;  ///< new or changed chunks, by key
    std::vector<ChunkKey> remove;       ///< chunks no probe needs any more, by key
};

/// Where a body's colliders go. engine/physics implements it (VCH-113): cook the meshes on workers, install them at
/// install_tick in key order, replace shapes in place and wake the bodies over a swapped chunk (ADR 0077 rule 5).
class ColliderSink {
public:
    virtual ~ColliderSink() = default;
    virtual void submit(ColliderUpdate update) = 0;
};

struct ColliderStats {
    std::uint32_t wanted = 0;     ///< LOD0 chunks within reach of a probe that may hold a surface
    std::uint32_t meshed = 0;     ///< LOD0 chunks meshed by this call
    std::uint32_t installed = 0;  ///< colliders in this call's update
    std::uint32_t removed = 0;
};

/// The weld check (SP-08, SP-09): every edge of a closed surface is shared by exactly two triangles. Edges are by
/// lattice position, so a crack between chunks shows as open edges. A debug and test tool.
struct WeldReport {
    std::uint64_t edges = 0;
    std::uint64_t open = 0;          ///< used by one triangle
    std::uint64_t non_manifold = 0;  ///< used by three or more
    std::uint64_t triangles = 0;
};
/// With `domain`, edges lying in one of its boundary planes are not counted: a flat world's surface ends there.
[[nodiscard]] WeldReport weld_check(std::span<const ChunkMesh* const> meshes,
                                    const std::optional<LatticeBox>& domain = std::nullopt);

class TerrainBody {
public:
    TerrainBody(std::shared_ptr<const DensitySource> source, TerrainSettings settings);
    ~TerrainBody();
    TerrainBody(const TerrainBody&) = delete;
    TerrainBody& operator=(const TerrainBody&) = delete;

    /// Replaces TerrainSettings::payload for chunks meshed from now on (waits for jobs in flight).
    void set_payload(ChunkPayload payload);
    /// Where the viewers are, body-local meters. Kept until changed.
    void set_viewers(std::span<const std::array<double, 3>> viewers);
    /// One streaming step (see the top of the file). Call once per frame from the job system's owner thread.
    StreamStats update(JobSystem& jobs);
    /// Finishes the jobs in flight and collects their results (before an edit, a save or teardown).
    void wait();

    /// The last complete selection, by key.
    [[nodiscard]] std::span<const ChunkRef> drawn() const { return drawn_; }
    /// Chunks meshed since the previous update (now held), and chunks dropped (no longer drawn or selected).
    [[nodiscard]] std::span<const ChunkRef> ready() const { return ready_; }
    [[nodiscard]] std::span<const ChunkRef> evicted() const { return evicted_; }
    /// mesh_hash of every drawn chunk, folded in key order.
    [[nodiscard]] std::uint64_t drawn_hash() const;
    /// weld_check over the drawn set, within the octree's domain.
    [[nodiscard]] WeldReport weld() const;
    [[nodiscard]] double lambda() const { return lambda_; }

    /// Applies a brush op (ADR 0054): waits for jobs in flight, edits the store, and marks every held chunk and
    /// collider whose samples it may have changed for remeshing. Drawn chunks stay drawn until their remesh lands.
    [[nodiscard]] Result<EditResult> apply(const BrushOp& op, JobSystem& jobs);
    [[nodiscard]] const EditStore& edits() const { return edits_; }
    /// Puts back a saved override record (EditStore::restore) and marks what it covers for remeshing.
    bool restore(const OverrideRecord& record);

    /// Where collider updates go (null: nowhere; the meshes are still kept for the next sink).
    void set_collider_sink(ColliderSink* sink) { sink_ = sink; }
    /// Colliders for the probes at `tick` (ADR 0053): meshes the LOD0 chunks they need and their neighbors on `jobs`,
    /// builds each chunk's ring, and submits what changed, to be installed before tick + collider_delay_ticks.
    ColliderStats update_colliders(std::span<const ColliderProbe> probes, std::uint64_t tick, JobSystem& jobs);
    /// The colliders installed so far (key order), for tests and tools.
    [[nodiscard]] const std::map<ChunkKey, std::uint64_t>& installed_colliders() const { return installed_; }

    /// Samples and meshes one chunk now, with the edits, on the calling thread (tools, tests, benchmarks).
    [[nodiscard]] ChunkRecord build(const ChunkSlot& slot) const;
    /// A chunk's samples with the edits (grid, apron and the faces in `face_mask`), on the calling thread.
    [[nodiscard]] ChunkSamples samples(const ChunkKey& key, std::uint8_t face_mask = 0) const;

    [[nodiscard]] const DensitySource& source() const { return *source_; }
    [[nodiscard]] const TerrainSettings& settings() const { return settings_; }
    [[nodiscard]] LodSelector& selector() { return selector_; }

private:
    struct Job;
    struct Entry {
        ChunkRef record;
        bool stale = false;  ///< an edit changed its samples: remesh, keep drawing it meanwhile
    };
    struct ColliderChunk {
        ChunkMesh mesh;
        std::uint64_t hash = 0;
        bool stale = false;
    };

    void collect(StreamStats* stats);
    void launch(std::vector<ChunkSlot> slots, JobSystem& jobs);
    void run_job(Job& job, IsoMesher& mesher) const;
    void classify(std::span<const ChunkKey> keys, std::span<BoxClass> out, JobSystem& jobs) const;
    [[nodiscard]] BoxClass classify_key(const ChunkKey& key) const;
    void invalidate(const LatticeBox& box);
    [[nodiscard]] IsoMesher& mesher(std::size_t thread);

    std::shared_ptr<const DensitySource> source_;
    TerrainSettings settings_;
    LodSelector selector_;
    EditStore edits_;
    std::vector<std::array<double, 3>> viewers_;
    double lambda_ = 4.0;

    std::unordered_map<ChunkSlot, Entry, ChunkSlotHash> cache_;
    std::vector<ChunkRef> drawn_, ready_, evicted_;
    std::vector<ChunkSlot> previous_target_;
    std::uint32_t stale_ = 0;
    std::uint64_t updates_ = 0;
    std::uint64_t serial_ = 0;

    std::vector<std::unique_ptr<IsoMesher>> meshers_;  ///< one per job thread
    std::unique_ptr<TaskGroup> group_;
    std::vector<std::unique_ptr<Job>> in_flight_;

    std::map<ChunkKey, ColliderChunk> collider_meshes_;
    std::map<ChunkKey, std::uint64_t> installed_;  ///< collider hash per installed chunk
    ColliderSink* sink_ = nullptr;
};

}  // namespace veach::voxel
