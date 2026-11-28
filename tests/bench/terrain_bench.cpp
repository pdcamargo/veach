// veach-bench-terrain (voxel.AC11, docs/specs/voxel-terrain/spec.md): chunks per second of engine/voxel's streaming
// jobs (sample a procgen field program, overlay edits, mesh) on 1, 4 and all threads, for SP-09's benchmark: every
// chunk drawn 100 m above an Earth-sized planet at lambda 4 (docs/research/spikes/sp-09-descent.md measured 129 /
// 457 / 1,678 chunks/s with its C++ generator). Run by hand on the release preset:
//   build/release/tests/veach-bench-terrain [--altitude M] [--scale S] [--repeats N]

#include "support/terrain_fields.hpp"

#include <veach/core/jobs.hpp>
#include <veach/voxel/terrain.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace veach;
using namespace veach::voxel;

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char** argv) {
    double altitude = 100.0, scale = 1.0;
    int repeats = 3;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--altitude") == 0) {
            altitude = std::strtod(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--scale") == 0) {
            scale = std::strtod(argv[i + 1], nullptr);
        } else if (std::strcmp(argv[i], "--repeats") == 0) {
            repeats = static_cast<int>(std::strtol(argv[i + 1], nullptr, 10));
        }
    }
    const auto density = test::planet(scale);
    // The viewer: `altitude` above the surface along a fixed direction (bisection on the unquantized field).
    const double len = std::sqrt(0.3 * 0.3 + 0.8 * 0.8 + 0.52 * 0.52);
    const std::array<double, 3> up{0.3 / len, 0.8 / len, 0.52 / len};
    double lo = density->settings().radius_m - 20'000.0 * scale - 100.0;
    double hi = density->settings().radius_m + 20'000.0 * scale + 100.0;
    for (int i = 0; i < 80; ++i) {
        const double mid = 0.5 * (lo + hi);
        (density->distance(up[0] * mid, up[1] * mid, up[2] * mid) < 0.0 ? lo : hi) = mid;
    }
    const double r = lo + altitude;
    const std::array<double, 3> viewer{up[0] * r, up[1] * r, up[2] * r};

    TerrainSettings settings;
    settings.octree = OctreeBounds::around_origin(density->max_extent() + 16.0);
    settings.lambda = 4.0;
    settings.budget.jobs_per_frame = 1'000'000;
    settings.asynchronous = false;
    TerrainBody body(density, settings);
    body.set_viewers(std::span(&viewer, 1));
    {
        JobSystem jobs(0);
        const double t0 = now_ms();
        const StreamStats first = body.update(jobs);
        std::printf("selection: %u chunks, %u boxes classified, select %.1f ms, first update %.1f ms on %u threads\n",
                    first.target, first.classified, first.select_ms, now_ms() - t0, jobs.thread_count());
        const StreamStats again = body.update(jobs);
        std::printf("reselect (cached classes): %.2f ms\n", again.select_ms);
    }
    const std::vector<ChunkRef> drawn(body.drawn().begin(), body.drawn().end());
    std::array<std::uint32_t, 32> per_level{};
    std::size_t empty = 0;
    std::uint64_t triangles = 0;
    for (const ChunkRef& c : drawn) {
        ++per_level[c->slot.key.level];
        empty += c->mesh.indices.empty() ? 1 : 0;
        triangles += c->mesh.triangle_count();
    }
    std::printf("drawn: %zu chunks (%zu without triangles), %llu triangles, root level %d, weld open edges %llu\n  ",
                drawn.size(), empty, static_cast<unsigned long long>(triangles), settings.octree.root_level,
                static_cast<unsigned long long>(body.weld().open));
    for (std::size_t l = 0; l < per_level.size(); ++l) {
        if (per_level[l] != 0) {
            std::printf("L%zu:%u ", l, per_level[l]);
        }
    }
    std::printf("\n%u noise octaves per sample\n\n", density->program().noise_octaves);

    std::printf("| Threads | Chunks/s | Sample ms per chunk (CPU) | Mesh ms per chunk |\n|---|---|---|---|\n");
    for (const std::uint32_t threads : {1u, 4u, default_job_threads()}) {
        JobSystem jobs(threads);
        std::vector<std::unique_ptr<IsoMesher>> meshers;
        for (std::uint32_t t = 0; t < jobs.thread_count(); ++t) {
            meshers.push_back(make_transvoxel_mesher({.encoding = density->encoding()}));
        }
        std::vector<double> sample_ms(drawn.size()), mesh_ms(drawn.size());
        double best = 1e30, best_sample = 0.0, best_mesh = 0.0;
        for (int rep = 0; rep < repeats; ++rep) {
            const double t0 = now_ms();
            jobs.parallel_for("bench: chunks", drawn.size(), 1, [&](std::size_t begin, std::size_t end) {
                IsoMesher& mesher = *meshers[jobs.thread_index()];
                for (std::size_t i = begin; i < end; ++i) {
                    const ChunkNeighbors n{.finer = drawn[i]->slot.finer};
                    const double a = now_ms();
                    const ChunkSamples s = body.samples(drawn[i]->slot.key, mesher.face_samples_needed(n));
                    const double b = now_ms();
                    ChunkMesh m;
                    if (s.has_surface) {
                        mesher.mesh(s, n, m);
                    }
                    sample_ms[i] = b - a;
                    mesh_ms[i] = now_ms() - b;
                }
            });
            const double ms = now_ms() - t0;
            if (ms < best) {
                best = ms;
                best_sample = best_mesh = 0.0;
                for (std::size_t i = 0; i < drawn.size(); ++i) {
                    best_sample += sample_ms[i];
                    best_mesh += mesh_ms[i];
                }
            }
        }
        const double n = static_cast<double>(drawn.size());
        std::printf("| %u | %.0f | %.2f | %.3f |\n", threads, n / (best / 1000.0), best_sample / n, best_mesh / n);
    }
    return 0;
}
