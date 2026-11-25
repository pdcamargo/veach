// Script system throughput benchmark (scripting spec, "Performance").
//   build/release/tests/veach-bench-script [entities=100000] [frames=30]
// Prints the mean frame time of a headless run whose only work is one Luau
// system over `entities` rows, for a read-only and a read-write system.

#include <veach/veach.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

constexpr const char* setup = R"(
local veach = require("@veach")
local Pos = veach.component("Pos", { p = vector.create(0, 0, 0) })
local Vel = veach.component("Vel", { v = vector.create(1, 2, 3) })
local N = %d
%s
for i = 1, N do
	local e = veach.spawn()
	e:set(Pos, { p = vector.create(i, 0, 0) })
	e:set(Vel, { v = vector.create(1, 2, 3) })
end
)";

constexpr const char* empty_fn = R"(
veach.system("Empty", "Simulation", veach.query(Pos, Vel), function(_e, _pos, _vel) end)
)";

constexpr const char* read_only = R"(
local sum = 0
veach.system("Read", "Simulation", veach.query(Pos, Vel), function(_e, pos, vel)
	sum += pos.p.x + vel.v.y
end)
)";

constexpr const char* read_write = R"(
veach.system("Move", "Simulation", veach.query(Pos, Vel), function(e, pos, vel)
	e:set(Pos, { p = pos.p + vel.v })
end)
)";

double run(const char* label, const char* system, int entities, int frames) {
    const auto dir = std::filesystem::temp_directory_path() / "veach-bench-script";
    std::filesystem::create_directories(dir);
    char source[4096];
    std::snprintf(source, sizeof(source), setup, entities, system);
    if (!veach::fs::write_file(dir / "bench.luau", source)) {
        return -1.0;
    }
    veach::EngineConfig config;
    config.app.name = "veach-bench-script";
    config.project_dir = dir;
    config.project.headless = true;
    config.project.frames = static_cast<std::uint64_t>(frames) + 1;
    config.project.script = "bench.luau";
    config.project.log_level = veach::log::Level::warn;
    veach::Engine engine(std::move(config));
    if (!engine.init() || !engine.start()) {
        return -1.0;
    }
    engine.frame();  // warm-up
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        engine.frame();
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
    std::printf("%-11s %7d entities: %8.2f ms/frame  %6.1f ns/entity\n", label, entities, ms, ms * 1e6 / entities);
    engine.shutdown();
    return ms;
}

}  // namespace

int main(int argc, char** argv) {
    const int entities = argc > 1 ? std::atoi(argv[1]) : 100000;
    const int frames = argc > 2 ? std::atoi(argv[2]) : 30;
    const bool ok = run("empty fn", empty_fn, entities, frames) >= 0 &&
                    run("read-only", read_only, entities, frames) >= 0 &&
                    run("read-write", read_write, entities, frames) >= 0;
    return ok ? 0 : 1;
}
