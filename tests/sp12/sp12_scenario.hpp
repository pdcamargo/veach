// Spike SP-12 (VCH-088), throwaway: the dig-and-collide run shared by the tests and the bench
// (docs/research/spikes/sp-12-dig-collide.md). A ball dropped at the dig site lands on the terrain; at a fixed tick
// the edit log digs a pit under it; the changed chunks are remeshed and their colliders recooked on workers and
// installed K ticks later, in key order; the ball falls in and comes to rest.
#pragma once

#include "sp12/sp12_terrain.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace veach::sp12 {

/// The tick of the edit log's second batch.
inline constexpr std::uint64_t second_dig_tick = 600;

/// The committed edit log. At tick 90: a pit under the ball, a second sphere deepening it off-center, a mound beside
/// it and a dent in the mound (add and subtract don't commute: the log is ordered), and a cavity deep in rock the
/// bounds had proven solid. At second_dig_tick: a sump under the ball, asleep at the pit's bottom by then. Seq 1..6.
std::vector<BrushOp> dig_log(const Site& site = default_site);

struct ScenarioConfig {
    Site site = default_site;   ///< where the ball drops and the log digs (the seams sweep moves it)
    std::uint32_t workers = 1;  ///< veach::JobSystem threads for sampling, edits, meshing and cooking
    int half = 2;               ///< the region: (2 half + 1)³ LOD0 chunks around the dig site's chunk
    int steps = 1200;           ///< 20 s: the ball has fallen into the sump and sleeps
    bool dig = true;            ///< false: the control, no edits
    bool reverse = false;       ///< apply each op to its chunks in reverse key order
    bool keep_meshes = false;   ///< fill ScenarioResult::meshes, origin, site and ball (the bench's screenshots)
};

struct ScenarioResult {
    std::uint64_t base_samples_hash = 0;  ///< generated, quantized samples of the region (ADR 0054's base_hash)
    std::uint64_t base_mesh_hash = 0;
    std::uint64_t samples_hash = 0;  ///< after the edit log
    std::uint64_t mesh_hash = 0;
    std::uint64_t colliders_hash = 0;        ///< every cooked shape's hash, in key order, after the edit log
    std::uint64_t install_hash = 0;          ///< terrain bodies after the last install
    std::vector<std::uint64_t> step_hashes;  ///< ball state after each step
    std::uint64_t chain = 0;
    std::uint64_t jolt_state = 0;  ///< Jolt's SaveState after the last step
    std::size_t sampled = 0, meshed = 0, changed = 0, materialized = 0, outside = 0, mismatches = 0;
    std::size_t terrain_bodies = 0;
    std::size_t triangles = 0, open_edges = 0;  ///< SP-08's weld check over every chunk after the edits
    std::size_t wound_out = 0;                  ///< triangles whose winding agrees with their vertex normals
    /// Each batch of the log: the ball relative to the dig site along radial up just before it (negative is below the
    /// original surface), whether the ball slept, and the chunks it changed.
    struct Batch {
        double depth_before = 0.0;
        bool asleep_before = false;
        std::size_t chunks = 0;
    };
    std::array<Batch, 2> batches{};
    /// At the end, the ball relative to the dig site: along radial up and across it.
    double depth = 0.0;
    double lateral = 0.0;
    double speed = 0.0;
    std::vector<ChunkMesh> meshes;         ///< with ScenarioConfig::keep_meshes
    std::array<std::int64_t, 3> origin{};  ///< the scene origin, LOD0 units
    std::array<double, 3> site{}, ball{};  ///< relative to the scene origin, meters
};

ScenarioResult run_scenario(const ScenarioConfig& config);

/// Steps whose ball hashes the tests commit (1-based).
inline constexpr int snapshot_steps[] = {60, 120, 600, 1200};

}  // namespace veach::sp12
