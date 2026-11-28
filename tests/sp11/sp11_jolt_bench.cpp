// Spike SP-11 (VCH-087), throwaway: step times and hashes for the findings (docs/research/spikes/sp-11-jolt.md).
//   veach-bench-sp11 [--bodies N]... [--workers N]... [--steps N]
// Runs every (bodies, workers) pair at the origin and 6,371 km out, prints the hashes, ms per step and how far the
// offset run drifted from the origin run. Build it in Release for the timings.
//   veach-bench-sp11 --sweep [--bodies N] [--mesh] [--boxes]
// The same scene from 1 km to 10,000 km out, against the origin scene with every body nudged by 1e-12 to 1e-6 m.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

#include "sp11/sp11_scene.hpp"

namespace {

int parse_int(const char* text) {
    int value = 0;
    const std::string_view s = text;
    std::from_chars(s.data(), s.data() + s.size(), value);
    return value;
}

double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[index];
}

double mean(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double v : values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

/// Mean and max distance between the final relative positions of two runs.
void drift(const veach::sp11::SceneResult& a, const veach::sp11::SceneResult& b, double& mean_d, double& max_d) {
    double sum = 0.0;
    max_d = 0.0;
    for (std::size_t i = 0; i + 2 < a.positions.size(); i += 3) {
        const double dx = a.positions[i] - b.positions[i];
        const double dy = a.positions[i + 1] - b.positions[i + 1];
        const double dz = a.positions[i + 2] - b.positions[i + 2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        max_d = std::max(max_d, d);
        sum += d;
    }
    mean_d = sum / static_cast<double>(a.positions.size() / 3);
}

/// What the sweep varies besides the distance.
struct SweepConfig {
    int bodies = 200;
    int steps = 600;
    veach::sp11::Ground ground = veach::sp11::Ground::heightfield;
    bool boxes_only = false;
};

/// The scene at growing distances from the origin (along the same 3-4-5 direction), one worker, then the origin scene
/// with every body nudged: whether the offset run's drift is a large-world effect or the pile's chaos.
int sweep(const SweepConfig& sweep_config) {
    const auto config_for = [&](double distance, double nudge) {
        veach::sp11::SceneConfig config;
        config.bodies = sweep_config.bodies;
        config.steps = sweep_config.steps;
        config.ground = sweep_config.ground;
        config.boxes_only = sweep_config.boxes_only;
        config.offset[0] = distance * 0.6;
        config.offset[1] = distance * 0.8;
        config.nudge = nudge;
        return config;
    };
    std::printf(
        "| run | awake at end | fastest body m/s | drift from origin run: mean m | max m |\n"
        "|---|---|---|---|---|\n");
    const veach::sp11::SceneResult origin_run = veach::sp11::run_scene(config_for(0.0, 0.0));
    const auto row = [&](const char* label, double value, const veach::sp11::SceneResult& r) {
        double mean_d = 0.0, max_d = 0.0;
        drift(r, origin_run, mean_d, max_d);
        std::printf("| %s %.4g m | %d | %.4f | %.4g | %.4g |\n", label, value, r.active_bodies, r.max_speed, mean_d,
                    max_d);
    };
    row("distance", 0.0, origin_run);
    for (const double distance : {1e3, 1e4, 1e5, 1e6, 6'371'000.0, 1e7}) {
        row("distance", distance, veach::sp11::run_scene(config_for(distance, 0.0)));
    }
    // About the rounding error of placing a body 6,371 km out (ulp 9.3e-10 m), and around it.
    for (const double nudge : {1e-12, 1e-9, 1e-6}) {
        row("origin, every body moved", nudge, veach::sp11::run_scene(config_for(0.0, nudge)));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> body_counts;
    std::vector<int> worker_counts;
    SweepConfig sweep_config;
    bool run_sweep = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const bool has_value = i + 1 < argc;
        if (flag == "--bodies" && has_value) {
            body_counts.push_back(parse_int(argv[++i]));
        } else if (flag == "--workers" && has_value) {
            worker_counts.push_back(parse_int(argv[++i]));
        } else if (flag == "--steps" && has_value) {
            sweep_config.steps = parse_int(argv[++i]);
        } else if (flag == "--sweep") {
            run_sweep = true;
        } else if (flag == "--mesh") {
            sweep_config.ground = veach::sp11::Ground::mesh;
        } else if (flag == "--boxes") {
            sweep_config.boxes_only = true;
        } else {
            std::fprintf(stderr,
                         "usage: veach-bench-sp11 [--bodies N]... [--workers N]... [--steps N] "
                         "[--sweep [--mesh] [--boxes]]\n");
            return 2;
        }
    }
    const int steps = sweep_config.steps;
    if (run_sweep) {
        if (!body_counts.empty()) {
            sweep_config.bodies = body_counts.front();
        }
        return sweep(sweep_config);
    }
    if (body_counts.empty()) {
        body_counts = {200, 2000};
    }
    if (worker_counts.empty()) {
        worker_counts = {0, 1, 4, 8};
    }

    std::printf(
        "| bodies | scene | workers | chain | step 600 | Jolt SaveState | mean ms | median | p95 | max | "
        "awake at end |\n|---|---|---|---|---|---|---|---|---|---|---|\n");
    bool all_equal = true;
    for (const int bodies : body_counts) {
        veach::sp11::SceneResult origin_run;
        for (const bool offset : {false, true}) {
            std::uint64_t first_chain = 0;
            for (std::size_t w = 0; w < worker_counts.size(); ++w) {
                veach::sp11::SceneConfig config;
                config.bodies = bodies;
                config.steps = steps;
                config.workers = worker_counts[w];
                config.time_steps = true;
                if (offset) {
                    config.offset[0] = 3'822'600.0;  // 6,371 km from the origin: 0.6 and 0.8 of it on x and y.
                    config.offset[1] = 5'096'800.0;
                }
                const veach::sp11::SceneResult r = veach::sp11::run_scene(config);
                if (w == 0) {
                    first_chain = r.chain;
                } else if (r.chain != first_chain) {
                    all_equal = false;
                }
                std::printf("| %d | %s | %d | 0x%016llx | 0x%016llx | 0x%016llx | %.3f | %.3f | %.3f | %.3f | %d |%s\n",
                            bodies, offset ? "6,371 km" : "origin", config.workers,
                            static_cast<unsigned long long>(r.chain),
                            static_cast<unsigned long long>(r.step_hashes.back()),
                            static_cast<unsigned long long>(r.jolt_state), mean(r.step_ms), percentile(r.step_ms, 0.5),
                            percentile(r.step_ms, 0.95), percentile(r.step_ms, 1.0), r.active_bodies,
                            r.update_errors != 0 ? " update errors" : "");
                if (!offset && w == 0) {
                    origin_run = r;
                } else if (offset && w == 0) {
                    // Drift between the two scenes, relative positions, after the last step.
                    double max_d = 0.0;
                    double sum_d = 0.0;
                    for (std::size_t b = 0; b + 2 < r.positions.size(); b += 3) {
                        const double dx = r.positions[b] - origin_run.positions[b];
                        const double dy = r.positions[b + 1] - origin_run.positions[b + 1];
                        const double dz = r.positions[b + 2] - origin_run.positions[b + 2];
                        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                        max_d = std::max(max_d, d);
                        sum_d += d;
                    }
                    int first_diff = 0;
                    while (first_diff < steps && r.step_hashes[static_cast<std::size_t>(first_diff)] ==
                                                     origin_run.step_hashes[static_cast<std::size_t>(first_diff)]) {
                        ++first_diff;
                    }
                    std::fprintf(stderr,
                                 "bodies %d: origin vs 6,371 km: first differing step %d; after step %d: mean drift "
                                 "%.6g m, max %.6g m\n",
                                 bodies, first_diff + 1, steps, sum_d / static_cast<double>(r.positions.size() / 3),
                                 max_d);
                }
            }
        }
    }
    std::printf("\nsame chain for every worker count: %s\n", all_equal ? "yes" : "NO");
    return all_equal ? 0 : 1;
}
