// Transform propagation cost (docs/specs/transforms/spec.md, docs/specs/frames/spec.md).
//   build/release/tests/veach-bench-transforms [passes=200]
// 100,000 entities, single thread, three shapes: a flat scene (every entity a root), wide trees (1k roots x 9 x 10,
// depth 3) and deep chains (10k chains x 10). Prints ms per pass of the engine's TransformPropagation phase.

#include <veach/veach.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

namespace math = veach::math;

struct Node {
    int parent = -1;
    math::vec3 translation{0.0f};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

std::uint32_t seed = 1;
float next() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) - 0.5f;
}

Node random_node(int parent) {
    return {.parent = parent,
            .translation = math::vec3(next(), next(), next()) * 10.0f,
            .rotation = math::angleAxis(next() * 6.0f, math::normalize(math::vec3(next(), next(), 1.0f)))};
}

/// `roots` roots, each with `fan1` children, each with `fan2` children. Parents come before children.
std::vector<Node> make_tree(int roots, int fan1, int fan2) {
    std::vector<Node> nodes;
    for (int r = 0; r < roots; ++r) {
        nodes.push_back(random_node(-1));
        const int root = static_cast<int>(nodes.size()) - 1;
        for (int i = 0; i < fan1; ++i) {
            nodes.push_back(random_node(root));
            const int child = static_cast<int>(nodes.size()) - 1;
            for (int j = 0; j < fan2; ++j) {
                nodes.push_back(random_node(child));
            }
        }
    }
    return nodes;
}

std::vector<Node> make_chains(int chains, int depth) {
    std::vector<Node> nodes;
    for (int c = 0; c < chains; ++c) {
        int parent = -1;
        for (int d = 0; d < depth; ++d) {
            nodes.push_back(random_node(parent));
            parent = static_cast<int>(nodes.size()) - 1;
        }
    }
    return nodes;
}

double bench(const char* label, const std::vector<Node>& nodes, int passes) {
    veach::World world;
    world.import<veach::CoreModule>();
    veach::PhaseRunner runner;
    runner.register_phases(world);
    std::vector<veach::Entity> entities;
    entities.reserve(nodes.size());
    for (const Node& n : nodes) {
        veach::Entity e = world.entity();
        if (n.parent >= 0) {
            e.child_of(entities[static_cast<std::size_t>(n.parent)]);
        }
        veach::Transform t;
        t.translation = n.translation;
        t.rotation = n.rotation;
        e.set(t);
        entities.push_back(e);
    }
    const auto pass = [&] {
        runner.run(world, veach::PhaseId::transform_propagation, 1.0f / 60.0f);
    };
    pass();  // warm-up
    double best = 1e300;
    for (int round = 0; round < 5; ++round) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < passes / 5; ++i) {
            pass();
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / (passes / 5);
        best = ms < best ? ms : best;
    }
    std::printf("%-20s %6zu entities: %7.3f ms per pass (best of 5 rounds of %d)\n", label, nodes.size(), best,
                passes / 5);
    std::fflush(stdout);
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    const int passes = argc > 1 ? std::max(5, std::atoi(argv[1])) : 200;
    std::vector<Node> flat;
    for (int i = 0; i < 100000; ++i) {
        flat.push_back(random_node(-1));
    }
    bench("flat (100k roots)", flat, passes);
    bench("1k roots x 9 x 10", make_tree(1000, 9, 10), passes);
    bench("10k chains x 10", make_chains(10000, 10), passes);
    return 0;
}
