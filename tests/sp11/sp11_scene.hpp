// Spike SP-11 (VCH-087), throwaway: Jolt in double precision and cross-platform deterministic mode
// (docs/research/spikes/sp-11-jolt.md, docs/adr/0058-physics-jolt.md). A pile of dynamic bodies dropped into a static
// heightfield bowl, stepped at 60 Hz, its full state hashed after every step. Jolt types stay in sp11_scene.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace veach::sp11 {

enum class Ground { heightfield, mesh };

/// One run of the scene.
struct SceneConfig {
    Ground ground = Ground::heightfield;
    bool boxes_only = false;  ///< Only boxes (they come to rest; spheres and capsules keep rolling in the bowl).
    double nudge = 0.0;       ///< Added to every body's initial x, in meters: how chaotic the pile is (the bench).
    int bodies = 200;         ///< Dynamic bodies: boxes, spheres and capsules in turn.
    int steps = 600;          ///< Fixed steps of 1/60 s.
    /// Jolt worker threads. 0 is JobSystemSingleThreaded (everything on the calling thread); N > 0 is a
    /// JobSystemThreadPool with N threads (the calling thread also runs jobs while it waits).
    int workers = 0;
    /// Where the scene's origin sits, in meters (double). The ground and every body are placed relative to it, and
    /// hashed relative to it.
    double offset[3] = {0.0, 0.0, 0.0};
    bool time_steps = false;  ///< Measure each PhysicsSystem::Update with a steady clock (the bench).
};

/// The result of a run.
struct SceneResult {
    /// After each step: a hash of every dynamic body's position relative to the offset (double bits), rotation,
    /// linear and angular velocity (float bits), in body-creation order. step_hashes[i] is the state after step i + 1.
    std::vector<std::uint64_t> step_hashes;
    /// All step hashes chained, in order: one value for the whole run.
    std::uint64_t chain = 0;
    /// Hash of Jolt's own PhysicsSystem::SaveState after the last step (bodies, contact cache, warm-start impulses).
    /// Includes absolute positions, so it only compares runs at the same offset.
    std::uint64_t jolt_state = 0;
    /// Final positions relative to the offset, x y z per body, to measure how far two runs drifted apart.
    std::vector<double> positions;
    std::vector<double> step_ms;  ///< Wall time per step when SceneConfig::time_steps is set.
    int active_bodies = 0;        ///< Awake bodies after the last step.
    double max_speed = 0.0;       ///< Fastest body after the last step, m/s.
    int update_errors = 0;        ///< Steps where PhysicsSystem::Update reported an error (too many pairs, ...).
};

SceneResult run_scene(const SceneConfig& config);

/// Steps in the committed snapshots (1-based): the hash is checked after these steps.
inline constexpr int snapshot_steps[] = {1, 60, 300, 600};

}  // namespace veach::sp11
