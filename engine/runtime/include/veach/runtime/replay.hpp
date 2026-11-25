#pragma once

// Record and replay (docs/specs/record-replay/spec.md): what a run needs to be repeated exactly, as JSON Lines:
// a header (seed, script, fixed step), then per frame the fixed steps it ran, the platform events it applied
// and a hash of the simulation state after its steps.

#include <veach/core/error.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/platform/platform.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace veach {

inline constexpr std::string_view replay_format = "veach-replay-1";

struct ReplayHeader {
    std::string engine_version;
    std::string project;
    std::string script;
    std::uint64_t seed = 0;
    double fixed_step = 0.0;
};

/// The simulation state's hash, and one per component type to name what differs.
struct StateHash {
    std::uint64_t total = 0;
    std::map<std::string, std::uint64_t> components;
};

struct ReplayFrame {
    std::uint64_t frame = 0;
    std::uint32_t steps = 0;
    std::vector<PlatformEvent> events;
    StateHash state;
};

struct ReplayLog {
    ReplayHeader header;
    std::vector<ReplayFrame> frames;
};

/// FNV-1a over every entity's Scriptable components, serialized through reflection, in entity order.
[[nodiscard]] StateHash simulation_hash(const World& world);

/// Reads a whole recording; replay.invalid on anything it cannot use.
[[nodiscard]] Result<ReplayLog> read_replay(const std::filesystem::path& path);

class ReplayRecorder {
public:
    [[nodiscard]] static Result<ReplayRecorder> open(const std::filesystem::path& path, const ReplayHeader& header);
    void write(const ReplayFrame& frame);

private:
    std::ofstream out_;
};

}  // namespace veach
