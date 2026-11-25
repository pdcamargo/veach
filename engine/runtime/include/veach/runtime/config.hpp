#pragma once

// App configuration (docs/specs/engine-lifecycle/spec.md). Every app accepts
// the same core flags; each project has a veach.project.json validated by the
// generated schemas/veach.project.schema.json. CLI flags override the file.

#include <veach/core/error.hpp>
#include <veach/core/log.hpp>
#include <veach/ecs/flecs.hpp>

#include <nlohmann/json_fwd.hpp>
#include <veach/platform/input.hpp>
#include <veach/platform/window.hpp>
#include <veach/rhi/rhi.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veach {

inline constexpr std::string_view project_file_name = "veach.project.json";

/// Contents of veach.project.json. Reflected: the schema is generated from this.
/// One action from "input" in veach.project.json: `"jump": ["space", "w"]` (docs/specs/input/spec.md).
struct ActionBinding {
    std::string action;
    Key primary = Key::unknown;
    Key secondary = Key::unknown;
};

struct ProjectConfig {
    std::string name = "untitled";
    std::string script;  ///< Startup script, relative to the project directory. Empty: none.
    Window window;       ///< The primary window.
    bool headless = false;
    std::uint64_t frames = 0;  ///< Stop after this many frames. 0: run until quit.
    std::uint64_t seed = 0;
    log::Format log_format = log::Format::human;
    log::Level log_level = log::Level::info;
    rhi::Backend rhi = rhi::Backend::null;
    std::uint32_t frames_in_flight = 2;  ///< How far the CPU may run ahead of the GPU (ADR 0029).
    bool rest = false;                   ///< Serve the Flecs REST API / explorer on port 27750.
    /// "input": action -> one or two keys. Not a reflected member (reflection has no maps yet): parsed and
    /// schema-described by hand, see input_schema().
    std::vector<ActionBinding> input;
};

struct AppInfo {
    std::string name;                       ///< Executable name, for usage text and logs.
    std::filesystem::path default_project;  ///< Used when --project is absent. Empty: the working directory.
};

struct EngineConfig {
    AppInfo app;
    std::filesystem::path project_dir;
    ProjectConfig project;
    bool show_help = false;
    bool log_to_console = true;         ///< Tests turn this off and capture logs with a sink.
    bool wait_for_profiler = false;     ///< --wait-for-profiler: block in init until Tracy connects.
    std::filesystem::path screenshot;   ///< --screenshot: PNG of the last frame (needs --frames and a GPU RHI)
    std::filesystem::path write_types;  ///< --write-types: write this app's veach.d.luau and exit
    bool mcp = false;                   ///< --mcp: serve the Model Context Protocol on stdio (logs go to stderr)
};

/// Registers ProjectConfig and the types it uses, for JSON loading and schemas.
void register_config_types(World& world);
/// The JSON Schema of the "input" object, for the generated veach.project.schema.json.
[[nodiscard]] nlohmann::json input_schema(const World& world);

/// argv (without the program name) -> project file -> CLI overrides.
[[nodiscard]] Result<EngineConfig> resolve_config(const AppInfo& app, std::span<const std::string_view> args);

/// Loads a project file over `config` (fields absent from the file keep their values).
[[nodiscard]] Result<> load_project_file(const std::filesystem::path& file, ProjectConfig& config);

[[nodiscard]] std::string usage(const AppInfo& app);

}  // namespace veach
