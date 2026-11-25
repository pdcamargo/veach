#include <veach/runtime/config.hpp>

#include <veach/core/suggest.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/json.hpp>
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
};

const std::vector<std::string>& known_flags() {
    static const std::vector<std::string> flags{
        "--help",      "--headless", "--window", "--frames", "--seed", "--log-format",
        "--log-level", "--project",  "--script", "--rhi",    "--rest",
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
        } else if (flag == "--wait-for-profiler") {
            cli.wait_for_profiler = true;
        } else if (flag == "--frames" || flag == "--seed") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            auto n = parse_u64(flag, *v);
            if (!n) {
                return std::unexpected(n.error());
            }
            (flag == "--frames" ? cli.frames : cli.seed) = *n;
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
        } else if (flag == "--screenshot" || flag == "--write-types") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            (flag == "--screenshot" ? cli.screenshot : cli.write_types) = std::string(*v);
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

}  // namespace

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
        .member(&ProjectConfig::rest, "rest");
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
    return from_json(world, config, json, file.filename().generic_string());
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
    if (cli->screenshot) {
        config.screenshot = std::filesystem::absolute(*cli->screenshot);
    }
    if (cli->write_types) {
        config.write_types = std::filesystem::absolute(*cli->write_types);
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
    }
    if (cli->rest) {
        p.rest = true;
    }
    if (p.frames_in_flight < 1 || p.frames_in_flight > 4) {
        return make_error(ErrorCode::data_type_mismatch,
                          std::format("frames_in_flight is {}; it must be between 1 and 4", p.frames_in_flight),
                          "2 is the default: the CPU records one frame while the GPU renders the previous one.");
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
  --log-format human|json Console log format. json = one JSON object per line.
  --log-level LEVEL       trace, debug, info (default), warn or error.
  --project PATH          Project directory (or its veach.project.json).
  --script PATH           Startup Luau script, relative to the project directory.
  --rhi null|metal|dx12   GPU backend (default: null).
  --rest                  Serve the Flecs explorer API on http://localhost:27750.
  --wait-for-profiler     Profile builds: wait (up to 30 s) for a Tracy capture to connect before init.
  --screenshot PATH       Write the last frame as a PNG (needs --frames N and --rhi metal). Works headless.
  --write-types PATH      Write this app's veach.d.luau (engine + app modules) and exit.
  --help                  Show this text.

Exit code: 0 when the run logged no errors, 1 otherwise.
)",
                       app.name);
}

}  // namespace veach
