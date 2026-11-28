// veach — command-line tools.
//   veach types   [--out FILE | --check FILE]   Luau definitions for require("@veach")
//   veach schemas [--out DIR  | --check DIR]    JSON Schemas for data files
// Both generate from reflection. --check exits 1 if the file on disk is stale.

#include "imgdiff.hpp"

#include <veach/veach.hpp>

#include <veach/platform/fs.hpp>
#include <veach/script/luau_types.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view usage_text = R"(Usage: veach <command> [options]

Commands:
  types   [--out FILE | --check FILE]  Generate veach.d.luau from reflection (default: print to stdout).
  schemas [--out DIR  | --check DIR]   Generate JSON Schemas for veach.project.json and .meta files.
  imgdiff <reference.png> <test.png> [--max-mean 0.01] [--diff out.png]
                                       Perceptual (NVIDIA FLIP) image comparison for golden tests.
  help                                 Show this text.

--check compares against the file(s) on disk and exits 1 if they are out of date.
)";

struct Output {
    enum class Mode { print, write, check } mode = Mode::print;
    std::string path;
};

bool parse_output(const std::vector<std::string_view>& args, Output& out) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--out" || args[i] == "--check") && i + 1 < args.size()) {
            out.mode = args[i] == "--out" ? Output::Mode::write : Output::Mode::check;
            out.path = std::string(args[++i]);
        } else {
            std::fprintf(stderr, "veach: unexpected argument '%.*s'\n%.*s", static_cast<int>(args[i].size()),
                         args[i].data(), static_cast<int>(usage_text.size()), usage_text.data());
            return false;
        }
    }
    return true;
}

/// Writes, checks or prints each generated file (keyed by full path). Returns the exit code.
int emit(const Output& out, std::string_view command, const std::map<veach::fs::Path, std::string>& files) {
    int code = 0;
    for (const auto& [path, contents] : files) {
        switch (out.mode) {
            case Output::Mode::print: std::fputs(contents.c_str(), stdout); break;
            case Output::Mode::write:
                if (auto written = veach::fs::write_file(path, contents); !written) {
                    std::fprintf(stderr, "veach: %s\n", written.error().to_string().c_str());
                    code = 1;
                } else {
                    std::printf("wrote %s\n", path.generic_string().c_str());
                }
                break;
            case Output::Mode::check: {
                auto existing = veach::fs::read_file(path);
                if (!existing || *existing != contents) {
                    std::printf("%s is out of date. Regenerate it with: veach %.*s --out %s\n",
                                path.generic_string().c_str(), static_cast<int>(command.size()), command.data(),
                                out.path.c_str());
                    code = 1;
                } else {
                    std::printf("%s is up to date\n", path.generic_string().c_str());
                }
                break;
            }
        }
    }
    return code;
}

int command_types(const std::vector<std::string_view>& args) {
    Output out;
    if (!parse_output(args, out)) {
        return 2;
    }
    // Boot a headless engine so every engine module registers its reflection,
    // exactly as a running app would see it.
    veach::EngineConfig config;
    config.app.name = "veach";
    config.project.headless = true;
    config.project.log_level = veach::log::Level::warn;
    veach::Engine engine(std::move(config));
    if (auto initialized = engine.init(); !initialized) {
        std::fprintf(stderr, "veach: %s\n", initialized.error().to_string().c_str());
        return 1;
    }
    const std::string types = veach::generate_luau_types(engine.world());
    engine.shutdown();
    return emit(out, "types", {{veach::fs::Path(out.path), types}});
}

int command_schemas(const std::vector<std::string_view>& args) {
    Output out;
    if (!parse_output(args, out)) {
        return 2;
    }
    veach::World world;
    veach::register_config_types(world);
    world.import<veach::AssetModule>();

    const auto schema = [&](veach::ecs::entity_t type, const void* defaults, std::string_view id,
                            std::string_view description) {
        nlohmann::json s = veach::json_schema(world, type, defaults);
        nlohmann::json doc = {
            {"$schema", "https://json-schema.org/draft/2020-12/schema"},
            {"$id", id},
            {"description", description},
        };
        doc.update(s);
        doc["properties"]["$schema"] = {{"type", "string"}};
        if (type == world.id<veach::ProjectConfig>()) {
            doc["properties"]["input"] = veach::input_schema(world);         // not a reflected member (no maps yet)
            doc["properties"]["axes"] = veach::axes_schema(world);           // nor this
            doc["properties"]["dev_scripts"] = veach::dev_scripts_schema();  // nor this (no arrays yet)
        }
        return doc.dump(2) + "\n";
    };

    const veach::ProjectConfig project_defaults;
    const veach::AssetMeta meta_defaults;
    const veach::fs::Path dir(out.path);
    const std::map<veach::fs::Path, std::string> files{
        {dir / "veach.project.schema.json",
         schema(world.id<veach::ProjectConfig>(), &project_defaults, "veach.project.schema.json",
                "Veach project file (veach.project.json). Generated by `veach schemas` from reflection.")},
        {dir / "asset.meta.schema.json",
         schema(world.id<veach::AssetMeta>(), &meta_defaults, "asset.meta.schema.json",
                "Veach asset sidecar (<asset>.meta). Generated by `veach schemas` from reflection.")},
    };
    return emit(out, "schemas", files);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        std::fputs(usage_text.data(), stdout);
        return args.empty() ? 2 : 0;
    }
    const std::vector<std::string_view> rest(args.begin() + 1, args.end());
    if (args[0] == "types") {
        return command_types(rest);
    }
    if (args[0] == "schemas") {
        return command_schemas(rest);
    }
    if (args[0] == "imgdiff") {
        return command_imgdiff(rest);
    }
    std::fprintf(stderr, "veach: unknown command '%.*s'\n%.*s", static_cast<int>(args[0].size()), args[0].data(),
                 static_cast<int>(usage_text.size()), usage_text.data());
    return 2;
}
