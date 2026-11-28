#include <veach/runtime/config.hpp>
#include <veach/runtime/replay.hpp>

#include <veach/core/jobs.hpp>
#include <veach/core/suggest.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/json.hpp>
#include <veach/ecs/reflection.hpp>
#include <veach/platform/fs.hpp>
#include <veach/platform/platform_module.hpp>

#include <nlohmann/json.hpp>

#include <charconv>
#include <format>
#include <optional>
#include <vector>

namespace veach {

namespace {

struct CliOverrides {
    std::optional<bool> headless;
    std::optional<std::uint64_t> frames;
    std::optional<std::uint64_t> seed;
    std::optional<std::uint64_t> jobs;
    std::optional<log::Format> log_format;
    std::optional<log::Level> log_level;
    std::optional<std::string> project;
    std::optional<std::string> script;
    std::optional<rhi::Backend> rhi;
    bool rest = false;
    bool help = false;
    bool wait_for_profiler = false;
    std::optional<std::string> screenshot;
    std::optional<std::string> write_types;
    std::optional<std::string> record;
    std::optional<std::string> replay;
    bool mcp = false;
};

const std::vector<std::string>& known_flags() {
    static const std::vector<std::string> flags{
        "--help",       "--headless",    "--window", "--frames", "--seed",   "--log-format",
        "--log-level",  "--project",     "--script", "--rhi",    "--rest",   "--wait-for-profiler",
        "--screenshot", "--write-types", "--mcp",    "--record", "--replay", "--jobs",
    };
    return flags;
}

std::unexpected<Error> invalid(std::string_view flag, std::string_view value, std::string_view expected) {
    return make_error(ErrorCode::cli_invalid_value, std::format("{} got '{}'", flag, value),
                      std::format("Expected {}.", expected));
}

Result<std::uint64_t> parse_u64(std::string_view flag, std::string_view text) {
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return invalid(flag, text, "a non-negative integer");
    }
    return value;
}

Result<CliOverrides> parse_args(std::span<const std::string_view> args) {
    CliOverrides cli;
    bool saw_headless = false;
    bool saw_window = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view flag = args[i];
        std::optional<std::string_view> inline_value;
        if (const auto eq = flag.find('='); flag.starts_with("--") && eq != std::string_view::npos) {
            inline_value = flag.substr(eq + 1);
            flag = flag.substr(0, eq);
        }
        const auto value = [&]() -> Result<std::string_view> {
            if (inline_value) {
                return *inline_value;
            }
            if (i + 1 >= args.size()) {
                return make_error(ErrorCode::cli_invalid_value, std::format("{} needs a value", flag),
                                  std::format("Write {} <value>.", flag));
            }
            return args[++i];
        };

        if (flag == "--help" || flag == "-h") {
            cli.help = true;
        } else if (flag == "--headless") {
            saw_headless = true;
            cli.headless = true;
        } else if (flag == "--window") {
            saw_window = true;
            cli.headless = false;
        } else if (flag == "--rest") {
            cli.rest = true;
        } else if (flag == "--mcp") {
            cli.mcp = true;
        } else if (flag == "--wait-for-profiler") {
            cli.wait_for_profiler = true;
        } else if (flag == "--frames" || flag == "--seed" || flag == "--jobs") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            auto n = parse_u64(flag, *v);
            if (!n) {
                return std::unexpected(n.error());
            }
            (flag == "--frames" ? cli.frames : flag == "--seed" ? cli.seed : cli.jobs) = *n;
        } else if (flag == "--log-format") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            if (*v == "human") {
                cli.log_format = log::Format::human;
            } else if (*v == "json") {
                cli.log_format = log::Format::json;
            } else {
                return invalid(flag, *v, "human or json");
            }
        } else if (flag == "--log-level") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            bool found = false;
            for (const auto level :
                 {log::Level::trace, log::Level::debug, log::Level::info, log::Level::warn, log::Level::error}) {
                if (*v == log::to_string(level)) {
                    cli.log_level = level;
                    found = true;
                }
            }
            if (!found) {
                return invalid(flag, *v, "trace, debug, info, warn or error");
            }
        } else if (flag == "--rhi") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            bool found = false;
            for (const auto backend : {rhi::Backend::null, rhi::Backend::metal, rhi::Backend::dx12}) {
                if (*v == rhi::to_string(backend)) {
                    cli.rhi = backend;
                    found = true;
                }
            }
            if (!found) {
                return invalid(flag, *v, "null, metal or dx12");
            }
        } else if (flag == "--screenshot" || flag == "--write-types" || flag == "--record" || flag == "--replay") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            (flag == "--screenshot"    ? cli.screenshot
             : flag == "--write-types" ? cli.write_types
             : flag == "--record"      ? cli.record
                                       : cli.replay) = std::string(*v);
        } else if (flag == "--project" || flag == "--script") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            (flag == "--project" ? cli.project : cli.script) = std::string(*v);
        } else {
            std::string hint = did_you_mean(flag, known_flags());
            hint += std::format("{}Run with --help for the list of flags.", hint.empty() ? "" : " ");
            return make_error(ErrorCode::cli_unknown_flag, std::format("unknown flag '{}'", flag), hint);
        }
    }
    if (saw_headless && saw_window) {
        return make_error(ErrorCode::cli_conflicting_flags, "--headless and --window were both given",
                          "Pass one of them.");
    }
    return cli;
}

/// "input": { "<action>": ["<key>", "<key>"?] }. Key names are veach::Key's constants.
Result<> parse_input(const World& world, const nlohmann::json& input, const std::string& context,
                     std::vector<ActionBinding>& out) {
    if (!input.is_object()) {
        return make_error(ErrorCode::data_type_mismatch, std::format("{}: 'input' must be an object", context),
                          R"(Map each action to its keys: "input": { "jump": ["space"] }.)");
    }
    const ecs::entity_t key_type = world.id<Key>();
    for (const auto& [action, keys] : input.items()) {
        if (!keys.is_array() || keys.empty() || keys.size() > 2) {
            return make_error(ErrorCode::data_type_mismatch,
                              std::format("{}: input.{} must be an array of one or two key names", context, action),
                              std::format(R"(For example "{}": ["space", "w"].)", action));
        }
        ActionBinding binding{.action = action};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            Key& key = i == 0 ? binding.primary : binding.secondary;
            const std::string name = keys[i].is_string() ? keys[i].get<std::string>() : keys[i].dump();
            if (!keys[i].is_string() || name == "unknown" || !write_enum(world, key_type, &key, name)) {
                return make_error(ErrorCode::input_unknown_key,
                                  std::format("{}: input.{}: unknown key '{}'", context, action, name),
                                  did_you_mean(name, enum_constants(world, key_type)));
            }
        }
        out.push_back(std::move(binding));
    }
    return {};
}

/// "axes": { "<action>": { "source": "<axis>", "negative": "<key>", "positive": "<key>", "dead_zone": 0.1,
/// "sensitivity": 1, "invert": false } }. Key and axis names get their own codes and suggestions; the rest is
/// reflection (AxisBinding).
Result<> parse_axes(const World& world, const nlohmann::json& axes, const std::string& context,
                    std::vector<AxisActionBinding>& out) {
    if (!axes.is_object()) {
        return make_error(ErrorCode::data_type_mismatch, std::format("{}: 'axes' must be an object", context),
                          R"(Map each analog action to its inputs: "axes": { "look_x": { "source": "mouse_x" } }.)");
    }
    out.clear();
    for (const auto& [action, entry] : axes.items()) {
        if (!entry.is_object()) {
            return make_error(ErrorCode::data_type_mismatch,
                              std::format("{}: axes.{} must be an object", context, action),
                              std::format(R"(For example "{}": {{ "source": "gamepad_left_x", "negative": "a", )"
                                          R"("positive": "d", "dead_zone": 0.15 }}.)",
                                          action));
        }
        for (const auto& [field, type, code] :
             {std::tuple{"source", world.id<Axis>(), ErrorCode::input_unknown_axis},
              std::tuple{"negative", world.id<Key>(), ErrorCode::input_unknown_key},
              std::tuple{"positive", world.id<Key>(), ErrorCode::input_unknown_key}}) {
            if (!entry.contains(field) || !entry[field].is_string()) {
                continue;  // absent, or a type mismatch from_json reports below
            }
            const std::string name = entry[field].get<std::string>();
            const char* none = code == ErrorCode::input_unknown_axis ? "none" : "unknown";
            std::uint64_t scratch = 0;  // wider than any enum
            if (name == none || !write_enum(world, type, &scratch, name)) {
                std::vector<std::string> names = enum_constants(world, type);
                std::erase(names, none);
                return make_error(code,
                                  std::format("{}: axes.{}.{}: unknown {} '{}'", context, action, field,
                                              code == ErrorCode::input_unknown_axis ? "axis" : "key", name),
                                  did_you_mean(name, names));
            }
        }
        AxisActionBinding binding{.action = action};
        if (auto read = from_json(world, binding.binding, entry, std::format("{}: axes.{}", context, action)); !read) {
            return read;
        }
        if (binding.binding.dead_zone < 0.0f || binding.binding.dead_zone >= 1.0f) {
            return make_error(
                ErrorCode::data_type_mismatch,
                std::format("{}: axes.{}.dead_zone is {}, outside [0, 1)", context, action, binding.binding.dead_zone),
                "A dead zone is the fraction of the axis that reads 0, such as 0.15 for a stick.");
        }
        out.push_back(std::move(binding));
    }
    return {};
}

/// "dev_scripts": ["scripts/dev/x.luau", ...].
Result<> parse_dev_scripts(const nlohmann::json& scripts, const std::string& context, std::vector<std::string>& out) {
    if (!scripts.is_array()) {
        return make_error(
            ErrorCode::data_type_mismatch, std::format("{}: 'dev_scripts' must be an array", context),
            R"(List script paths relative to the project: "dev_scripts": ["scripts/dev/fly_camera.luau"].)");
    }
    out.clear();
    for (const nlohmann::json& script : scripts) {
        if (!script.is_string() || script.get<std::string>().empty()) {
            return make_error(
                ErrorCode::data_type_mismatch,
                std::format("{}: dev_scripts: {} is not a script path", context, script.dump()),
                R"(Each entry is a path relative to the project, such as "scripts/dev/fly_camera.luau".)");
        }
        out.push_back(script.get<std::string>());
    }
    return {};
}

}  // namespace

nlohmann::json dev_scripts_schema() {
    return {
        {"type", "array"},
        {"description",
         "Scripts run after the startup script, in order, in every run: development tools that work "
         "on any scene, such as a fly camera."},
        {"items", {{"type", "string"}}},
        {"default", nlohmann::json::array()},
    };
}

nlohmann::json input_schema(const World& world) {
    std::vector<std::string> keys = enum_constants(world, world.id<Key>());
    std::erase(keys, "unknown");
    return {
        {"type", "object"},
        {"description", "Input actions: each action name maps to one or two keys (docs/specs/input/spec.md)."},
        {"additionalProperties", {{"type", "array"}, {"minItems", 1}, {"maxItems", 2}, {"items", {{"enum", keys}}}}},
        {"default", nlohmann::json::object()},
    };
}

nlohmann::json axes_schema(const World& world) {
    const AxisBinding defaults;
    nlohmann::json entry = json_schema(world, world.id<AxisBinding>(), &defaults);
    for (const char* field : {"negative", "positive"}) {
        nlohmann::json kept = nlohmann::json::array();
        for (const nlohmann::json& name : entry["properties"][field]["enum"]) {
            if (name != "unknown") {
                kept.push_back(name);
            }
        }
        entry["properties"][field]["enum"] = std::move(kept);
        entry["properties"][field].erase("default");  // unbound: leave the field out
    }
    entry["properties"]["dead_zone"]["minimum"] = 0;
    entry["properties"]["dead_zone"]["exclusiveMaximum"] = 1;
    entry["additionalProperties"] = false;
    return {
        {"type", "object"},
        {"description",
         "Analog input actions: each name maps to an analog source (mouse motion or wheel, a gamepad stick or "
         "trigger) and/or two keys as a digital fallback. Value = (sensitivity * dead-zoned source + positive - "
         "negative), negated if invert (docs/specs/input/spec.md)."},
        {"additionalProperties", std::move(entry)},
        {"default", nlohmann::json::object()},
    };
}

void register_config_types(World& world) {
    world.import<CoreModule>();
    world.import<PlatformModule>();
    world.component<log::Format>();
    world.component<log::Level>();
    world.component<rhi::Backend>();
    world.component<ProjectConfig>()
        .member(&ProjectConfig::name, "name")
        .member(&ProjectConfig::script, "script")
        .member(&ProjectConfig::window, "window")
        .member(&ProjectConfig::headless, "headless")
        .member(&ProjectConfig::frames, "frames")
        .member(&ProjectConfig::seed, "seed")
        .member(&ProjectConfig::log_format, "log_format")
        .member(&ProjectConfig::log_level, "log_level")
        .member(&ProjectConfig::rhi, "rhi")
        .member(&ProjectConfig::frames_in_flight, "frames_in_flight")
        .member(&ProjectConfig::jobs, "jobs")
        .member(&ProjectConfig::rest, "rest")
        .member(&ProjectConfig::hot_reload, "hot_reload");
}

Result<> load_project_file(const std::filesystem::path& file, ProjectConfig& config) {
    auto text = fs::read_file(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(*text);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(ErrorCode::data_parse_failed, std::format("{}: {}", file.generic_string(), e.what()),
                          "Fix the JSON syntax (trailing commas and comments are not allowed).");
    }
    World world;
    register_config_types(world);
    const std::string context = file.filename().generic_string();
    if (json.is_object() && json.contains("input")) {
        config.input.clear();
        if (auto parsed = parse_input(world, json["input"], context, config.input); !parsed) {
            return parsed;
        }
        json.erase("input");
    }
    if (json.is_object() && json.contains("axes")) {
        if (auto parsed = parse_axes(world, json["axes"], context, config.axes); !parsed) {
            return parsed;
        }
        json.erase("axes");
    }
    if (json.is_object() && json.contains("dev_scripts")) {
        if (auto parsed = parse_dev_scripts(json["dev_scripts"], context, config.dev_scripts); !parsed) {
            return parsed;
        }
        json.erase("dev_scripts");
    }
    return from_json(world, config, json, context);
}

Result<EngineConfig> resolve_config(const AppInfo& app, std::span<const std::string_view> args) {
    auto cli = parse_args(args);
    if (!cli) {
        return std::unexpected(cli.error());
    }
    EngineConfig config;
    config.app = app;
    config.show_help = cli->help;
    config.wait_for_profiler = cli->wait_for_profiler;
    config.mcp = cli->mcp;
    if (cli->screenshot) {
        config.screenshot = std::filesystem::absolute(*cli->screenshot);
    }
    if (cli->write_types) {
        config.write_types = std::filesystem::absolute(*cli->write_types);
    }
    if (cli->record) {
        config.record = std::filesystem::absolute(*cli->record);
    }
    if (cli->replay) {
        config.replay = std::filesystem::absolute(*cli->replay);
    }

    namespace stdfs = std::filesystem;
    if (cli->project) {
        stdfs::path path = stdfs::absolute(*cli->project);
        if (path.filename() == project_file_name) {
            path = path.parent_path();
        }
        if (!fs::exists(path / project_file_name)) {
            return make_error(ErrorCode::project_not_found,
                              std::format("no {} in '{}'", project_file_name, path.generic_string()),
                              "Pass the directory that contains veach.project.json (or the file itself).");
        }
        config.project_dir = path;
    } else {
        config.project_dir = app.default_project.empty() ? stdfs::current_path() : app.default_project;
    }

    const stdfs::path file = config.project_dir / project_file_name;
    if (fs::exists(file)) {
        if (auto loaded = load_project_file(file, config.project); !loaded) {
            return std::unexpected(loaded.error());
        }
    }

    ProjectConfig& p = config.project;
    if (cli->headless) {
        p.headless = *cli->headless;
    }
    if (cli->frames) {
        p.frames = *cli->frames;
    }
    if (cli->seed) {
        p.seed = *cli->seed;
    }
    if (cli->log_format) {
        p.log_format = *cli->log_format;
    }
    if (cli->log_level) {
        p.log_level = *cli->log_level;
    }
    if (cli->script) {
        p.script = *cli->script;
    }
    if (cli->rhi) {
        p.rhi = *cli->rhi;
    } else if (!p.headless && p.rhi == rhi::Backend::null) {
        // A window on the null RHI stays black: windowed runs default to the platform's GPU backend.
#if defined(__APPLE__)
        p.rhi = rhi::Backend::metal;
#elif defined(_WIN32)
        p.rhi = rhi::Backend::dx12;
#endif
    }
    if (cli->rest) {
        p.rest = true;
    }
    if (cli->jobs) {
        if (*cli->jobs > max_job_threads) {
            return invalid("--jobs", std::to_string(*cli->jobs),
                           std::format("0 (the hardware threads minus one) to {}", max_job_threads));
        }
        p.jobs = static_cast<std::uint32_t>(*cli->jobs);
    }
    // Record and replay (docs/specs/record-replay/spec.md): a replay takes its seed and script from the recording;
    // neither runs hot reload, which would make the run unrepeatable.
    if (!config.record.empty() && !config.replay.empty()) {
        return make_error(ErrorCode::cli_conflicting_flags, "--record and --replay together",
                          "Replay a recording, or record a new one; not both in one run.");
    }
    if (!config.record.empty() || !config.replay.empty()) {
        p.hot_reload = false;
    }
    if (!config.replay.empty()) {
        auto log = read_replay(config.replay);
        if (!log) {
            return std::unexpected(log.error());
        }
        if ((cli->seed && *cli->seed != log->header.seed) || (cli->script && *cli->script != log->header.script)) {
            return make_error(ErrorCode::replay_invalid,
                              std::format("the recording ran script '{}' with seed {}; this run asks for others",
                                          log->header.script, log->header.seed),
                              "Leave --script and --seed out: a replay takes them from the recording.");
        }
        p.seed = log->header.seed;
        p.script = log->header.script;
        if (p.frames == 0 || p.frames > log->frames.size()) {
            p.frames = log->frames.size();
        }
    }
    if (p.frames_in_flight < 1 || p.frames_in_flight > 4) {
        return make_error(ErrorCode::data_type_mismatch,
                          std::format("frames_in_flight is {}; it must be between 1 and 4", p.frames_in_flight),
                          "2 is the default: the CPU records one frame while the GPU renders the previous one.");
    }
    if (p.jobs > max_job_threads) {
        return make_error(ErrorCode::data_type_mismatch,
                          std::format("jobs is {}; it must be between 0 and {}", p.jobs, max_job_threads),
                          "0 (the default) uses the hardware threads minus one; 1 runs every job on the main thread.");
    }
    if (!config.screenshot.empty()) {
        if (p.frames == 0) {
            return make_error(ErrorCode::cli_conflicting_flags, "--screenshot needs --frames N",
                              "The screenshot is taken of the last frame, e.g. --frames 1 --screenshot out.png");
        }
        if (p.rhi == rhi::Backend::null) {
            return make_error(ErrorCode::cli_conflicting_flags, "--screenshot needs a GPU backend",
                              "The null RHI draws nothing. Add --rhi metal (macOS).");
        }
    }
    return config;
}

std::string usage(const AppInfo& app) {
    return std::format(R"(Usage: {} [flags]

Core flags (every Veach app accepts these; they override veach.project.json):
  --headless              No window, no GPU. Use for CI and agents.
  --window                Open the primary window (default unless the project says headless).
  --frames N              Run N frames, then shut down. 0 = until quit (default).
  --seed N                Seed for deterministic simulation randomness.
  --jobs N                Threads that run jobs, the main thread included. 0 = hardware threads - 1 (default),
                          1 = single-threaded. Results are the same for every N.
  --log-format human|json Console log format. json = one JSON object per line.
  --log-level LEVEL       trace, debug, info (default), warn or error.
  --project PATH          Project directory (or its veach.project.json).
  --script PATH           Startup Luau script, relative to the project directory.
  --rhi null|metal|dx12   GPU backend (default: null headless; metal or dx12 with a window).
  --rest                  Serve the Flecs explorer API on http://localhost:27750.
  --wait-for-profiler     Profile builds: wait (up to 30 s) for a Tracy capture to connect before init.
  --screenshot PATH       Write the last frame as a PNG (needs --frames N and --rhi metal). Works headless.
  --write-types PATH      Write this app's veach.d.luau (engine + app modules) and exit.
  --mcp                   Serve the Model Context Protocol on stdio for agents (logs go to stderr).
                          Headless, frames advance only through the step_frames tool.
  --record PATH           Record the run (seed, script, input, steps, state hashes) to replay it exactly.
  --replay PATH           Replay a recording, checking the simulation state every frame.
  --help                  Show this text.

Exit code: 0 when the run logged no errors, 1 otherwise.
)",
                       app.name);
}

}  // namespace veach
