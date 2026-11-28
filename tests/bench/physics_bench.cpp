// veach::physics: ms per step of SP-11's pile (boxes, spheres and capsules dropped into a bowl) on the engine's job
// system, per body count and thread count, with the state hash of each run (docs/specs/physics/spec.md, physics.AC11).
//   veach-bench-physics [--bodies N]... [--threads N]... [--cap N] [--steps N]   (--cap:
//   WorldSettings::max_concurrency)
// Build it in Release for the timings. The hash must be the same on every line of one body count.

#include <veach/core/jobs.hpp>
#include <veach/physics/physics.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

using namespace veach;
using namespace veach::physics;

int parse_int(const char* text) {
    int value = 0;
    const std::string_view s = text;
    std::from_chars(s.data(), s.data() + s.size(), value);
    return value;
}

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

double unit(std::uint64_t key) {
    return static_cast<double>(static_cast<std::int64_t>(mix(key) % 2001) - 1000) / 1000.0;
}

struct Run {
    std::vector<double> ms;
    std::uint64_t hash = 0;
    std::size_t awake = 0;
};

Run run(int count, std::uint32_t threads, std::uint32_t cap, int steps) {
    JobSystem jobs(threads);
    PhysicsWorld world({.jobs = &jobs, .max_concurrency = cap});
    const SceneId scene = *world.create_scene({.name = "pile",
                                               .max_bodies = static_cast<std::uint32_t>(count) + 16,
                                               .max_body_pairs = 65536,
                                               .max_contact_constraints = 65536,
                                               .temp_allocator_bytes = std::size_t{64} << 20});
    constexpr std::uint32_t n = 64;
    std::vector<float> heights(n * n);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const int dx = 2 * static_cast<int>(x) - 63, dz = 2 * static_cast<int>(z) - 63;
            heights[z * n + x] = static_cast<float>(dx * dx + dz * dz) / 512.0f;
        }
    }
    static_cast<void>(world.add_body(
        scene, {.shape = *Shape::height_field(heights, n, math::vec3(-31.5f, 0.0f, -31.5f), math::vec3(1.0f)),
                .motion = MotionType::static_body}));
    const Shape shapes[] = {Shape::box(math::vec3(0.4f, 0.3f, 0.5f)), Shape::sphere(0.45f), Shape::capsule(0.4f, 0.3f)};
    int side = 6;
    while (side * side * 10 < count) {
        ++side;
    }
    for (int i = 0; i < count; ++i) {
        const auto key = static_cast<std::uint64_t>(i) * 16;
        const int layer = i / (side * side), cell = i % (side * side);
        const double half = static_cast<double>(side - 1) / 2.0;
        static_cast<void>(
            world.add_body(scene, {.shape = shapes[i % 3],
                                   .position = {(static_cast<double>(cell % side) - half) * 1.5 + 0.1 * unit(key),
                                                4.0 + 1.5 * static_cast<double>(layer) + 0.1 * unit(key + 1),
                                                (static_cast<double>(cell / side) - half) * 1.5 + 0.1 * unit(key + 2)},
                                   .rotation = {1.5, static_cast<double>(static_cast<float>(unit(key + 3))),
                                                static_cast<double>(static_cast<float>(unit(key + 4))),
                                                static_cast<double>(static_cast<float>(unit(key + 5)))},
                                   .friction = 0.5f,
                                   .restitution = 0.1f}));
    }
    world.optimize(scene);
    world.set_gravity([](const GravityQuery&) { return math::dvec3(0.0, -9.81, 0.0); });
    Run r;
    for (int s = 0; s < steps; ++s) {
        const auto t0 = std::chrono::steady_clock::now();
        world.step();
        r.ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    r.hash = world.state_hash(scene);
    r.awake = world.scene_info(scene).awake;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> bodies;
    std::vector<std::uint32_t> threads;
    int steps = 600;
    std::uint32_t cap = max_job_threads;  // WorldSettings::max_concurrency; the engine's default is 4
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--bodies") {
            bodies.push_back(parse_int(argv[++i]));
        } else if (a == "--threads") {
            threads.push_back(static_cast<std::uint32_t>(parse_int(argv[++i])));
        } else if (a == "--cap") {
            cap = static_cast<std::uint32_t>(parse_int(argv[++i]));
        } else if (a == "--steps") {
            steps = parse_int(argv[++i]);
        }
    }
    if (bodies.empty()) {
        bodies = {200, 2000};
    }
    if (threads.empty()) {
        threads = {1, 2, 4, 8, default_job_threads()};
    }
    std::printf(
        "| Bodies | Threads | ms/step mean | p95 | Awake at the end | Jolt state hash |\n|---|---|---|---|---|---|\n");
    for (const int b : bodies) {
        for (const std::uint32_t t : threads) {
            Run r = run(b, t, cap, steps);
            double sum = 0.0;
            for (const double v : r.ms) {
                sum += v;
            }
            std::sort(r.ms.begin(), r.ms.end());
            std::printf("| %d | %u | %.3f | %.3f | %zu | %016llx |\n", b, t, sum / static_cast<double>(r.ms.size()),
                        r.ms[static_cast<std::size_t>(0.95 * static_cast<double>(r.ms.size() - 1))], r.awake,
                        static_cast<unsigned long long>(r.hash));
        }
    }
    return 0;
}
