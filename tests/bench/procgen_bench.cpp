// veach-bench-procgen (procgen.AC11, docs/specs/procgen/spec.md): samples per second of the canonical CPU interpreter.
//   veach-bench-procgen [--repeats 5] [--threads N]
// SP-06's terrain graph on a 1024^2 cube-face region (its domain and its numbers), then on lattice chunks of 35^3
// corner samples (a 32^3-cell chunk with its apron, the shape engine/voxel samples), and a cave-scale graph on the same
// chunks; one thread, then N threads on veach::JobSystem. Build with the release preset for the timings.

#include <veach/core/jobs.hpp>
#include <veach/procgen/field_graph.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace veach;
namespace pg = veach::procgen;

namespace {

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

constexpr std::string_view caves_json = R"({
  "veach_graph": 1, "id": "bench:caves", "inputs": { "p": "p_local" },
  "nodes": [
    { "id": "tunnels", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "m": 24 }, "octaves": 4 } },
    { "id": "rock", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "m": 3 }, "octaves": 3, "gain": 0.4 } },
    { "id": "walls", "type": "veach:ridge@1", "inputs": { "x": "tunnels" }, "params": { "offset": 0.6 } },
    { "id": "density", "type": "veach:add@1", "inputs": { "a": "walls", "b": "rock" } }
  ],
  "outputs": { "density": "density" }
})";

pg::FieldProgram compile_or_exit(std::string_view json, const pg::BodyConstants& body) {
    auto graph = pg::parse_field_graph(json);
    auto program = graph ? pg::compile(*graph, body) : Result<pg::FieldProgram>(std::unexpected(graph.error()));
    if (!program) {
        std::fprintf(stderr, "%s\n", program.error().message.c_str());
        std::exit(1);
    }
    return std::move(*program);
}

template <class Fn>
double best_of(int repeats, Fn&& fn) {
    double best = 1e30;
    for (int r = 0; r < repeats; ++r) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        best = std::min(best, seconds_since(start));
    }
    return best;
}

void row(const char* what, double samples, double seconds, std::uint32_t octaves, std::uint32_t threads) {
    const double rate = samples / seconds;
    std::printf("| %s | %u | %.2f M samples/s | %.2f M samples/s per thread | %.2f ns per noise octave |\n", what,
                threads, rate * 1e-6, rate * 1e-6 / threads, 1e9 / (rate / threads) / octaves);
}

}  // namespace

int main(int argc, char** argv) {
    int repeats = 5;
    std::uint32_t threads = default_job_threads();
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (flag == "--repeats") {
            repeats = std::atoi(argv[i + 1]);
        } else if (flag == "--threads") {
            threads = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        }
    }
    std::ifstream file(VEACH_SOURCE_DIR "/tests/procgen/terrain.field.json", std::ios::binary);
    const std::string terrain_json(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
    const pg::FieldProgram terrain = compile_or_exit(terrain_json, {.seed = 42, .radius_planet_m = 6.0e6});
    const pg::FieldProgram caves = compile_or_exit(caves_json, {.seed = 7});
    std::printf("| Workload | Threads | Throughput | Per thread | Per noise octave |\n|---|---|---|---|---|\n");

    // SP-06's measurement: the 1024^2 face, one thread, batches of 256 and of 1.
    pg::FaceRegion face;
    face.resolution = 1024;
    std::vector<float> out(face.size());
    const auto n_face = static_cast<double>(face.size());
    row("terrain, cube face 1024^2, batch 256", n_face, best_of(repeats, [&] { pg::evaluate(terrain, face, out); }),
        terrain.noise_octaves, 1);
    row("terrain, cube face 1024^2, batch 1", n_face,
        best_of(std::max(1, repeats / 2), [&] { pg::evaluate(terrain, face, out, {.batch = 1}); }),
        terrain.noise_octaves, 1);

    // Lattice chunks on a 6,000 km body's surface: 64 chunks of 35^3 corner samples at level 0.
    const pg::Lattice lod0{.spacing_m = 0.5};
    constexpr int chunks = 64;
    constexpr std::uint32_t edge = 35;
    std::vector<float> chunk(std::size_t{edge} * edge * edge);
    auto box = [&](int c) {
        return pg::LatticeBox{.first = {12'000'000 + 32 * (c % 4), 32 * ((c / 4) % 4), 32 * (c / 16)},
                              .step = 1,
                              .count = {edge, edge, edge}};
    };
    const double n_chunks = static_cast<double>(chunks) * static_cast<double>(chunk.size());
    for (const auto& [name, program] :
         {std::pair{"terrain, lattice chunks 35^3", &terrain}, std::pair{"caves, lattice chunks 35^3", &caves}}) {
        row(name, n_chunks,
            best_of(repeats,
                    [&] {
                        for (int c = 0; c < chunks; ++c) {
                            pg::evaluate(*program, lod0, box(c), chunk);
                        }
                    }),
            program->noise_octaves, 1);
        JobSystem jobs(threads);
        // Chunks in parallel, each chunk on one job: how engine/voxel will stream.
        std::vector<std::vector<float>> per(chunks, std::vector<float>(chunk.size()));
        row(name, n_chunks,
            best_of(repeats,
                    [&] {
                        jobs.parallel_for("chunks", chunks, 1, [&](std::size_t begin, std::size_t end) {
                            for (std::size_t c = begin; c < end; ++c) {
                                pg::evaluate(*program, lod0, box(static_cast<int>(c)), per[c]);
                            }
                        });
                    }),
            program->noise_octaves, jobs.thread_count());
        // One chunk cut into fixed jobs by evaluate(JobSystem&, ...).
        bool same = true;
        const double seconds = best_of(repeats, [&] {
            for (int c = 0; c < chunks; ++c) {
                pg::evaluate(jobs, *program, lod0, box(c), chunk);
            }
        });
        pg::evaluate(*program, lod0, box(chunks - 1), per[0]);
        same = std::memcmp(per[0].data(), chunk.data(), chunk.size() * sizeof(float)) == 0;
        row((std::string(name) + ", split per chunk").c_str(), n_chunks, seconds, program->noise_octaves,
            jobs.thread_count());
        if (!same) {
            std::printf("parallel and serial differ: FAIL\n");
            return 1;
        }
    }
    return 0;
}
