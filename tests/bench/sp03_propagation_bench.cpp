// Spike SP-03 (VCH-079), THROWAWAY: transform propagation in float vs double (ADR 0045).
//   build/release/tests/veach-bench-sp03-propagation [frames=200]
// 100k entities in a parent/child hierarchy, single thread. Prints ms per propagation pass for:
//   ecs   : the engine's propagate_transforms (Flecs cascade, float32 mat4) vs the same cascade over spike double
//           components (dmat4, and ADR 0045's FrameTransform: double translation + float32 3x3 basis);
//   flat  : the same math over arrays sorted parents-first, without the ECS (the math's own cost);
//   extract: building 100k camera-relative float32 draw matrices from each representation.
// Findings: docs/research/spikes/sp-03-precision-frames.md.

#include <veach/veach.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

namespace math = veach::math;

// ---- spike double components (ADR 0045) -------------------------------------------------------------------------

struct DTransform {  ///< the proposed Transform: double translation, float32 rotation and scale
    math::dvec3 translation{0.0};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    math::vec3 scale{1.0f};
};

struct DGlobal {  ///< variant 1: a full double matrix
    math::dmat4 matrix{1.0};
};

struct FrameTransform {  ///< variant 2, ADR 0045: double translation, float32 basis (rotation x scale)
    math::dvec3 translation{0.0};
    math::mat3 basis{1.0f};
};

math::mat3 basis_of(const math::quat& rotation, const math::vec3& scale) {
    math::mat3 m = math::mat3_cast(math::normalize(rotation));
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    return m;
}

math::dmat4 dmatrix_of(const DTransform& t) {
    math::dmat4 m(math::dmat3(basis_of(t.rotation, t.scale)));
    m[3] = math::dvec4(t.translation, 1.0);
    return m;
}

void compose_d(const DTransform& local, const DGlobal* parent, DGlobal& global) {
    global.matrix = parent ? parent->matrix * dmatrix_of(local) : dmatrix_of(local);
}

void compose_frame(const DTransform& local, const FrameTransform* parent, FrameTransform& global) {
    const math::mat3 basis = basis_of(local.rotation, local.scale);
    if (parent) {
        global.basis = parent->basis * basis;
        global.translation = parent->translation + math::dmat3(parent->basis) * local.translation;
    } else {
        global.basis = basis;
        global.translation = local.translation;
    }
}

// ---- the hierarchy -------------------------------------------------------------------------------------------------

struct Node {
    int parent = -1;
    math::vec3 translation{0.0f};
    math::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// `roots` roots, each with `fan1` children, each with `fan2` children. Parents come before children.
std::vector<Node> make_tree(int roots, int fan1, int fan2) {
    std::vector<Node> nodes;
    std::uint32_t seed = 1;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) - 0.5f;
    };
    const auto add = [&](int parent) {
        Node n{.parent = parent,
               .translation = math::vec3(next(), next(), next()) * 10.0f,
               .rotation = math::angleAxis(next() * 6.0f, math::normalize(math::vec3(next(), next(), 1.0f)))};
        nodes.push_back(n);
        return static_cast<int>(nodes.size()) - 1;
    };
    for (int r = 0; r < roots; ++r) {
        const int root = add(-1);
        for (int i = 0; i < fan1; ++i) {
            const int child = add(root);
            for (int j = 0; j < fan2; ++j) {
                add(child);
            }
        }
    }
    return nodes;
}

/// `chains` chains of `depth` entities each.
std::vector<Node> make_chains(int chains, int depth) {
    std::vector<Node> nodes;
    for (int c = 0; c < chains; ++c) {
        int parent = -1;
        for (int d = 0; d < depth; ++d) {
            nodes.push_back({.parent = parent,
                             .translation = math::vec3(0.5f, 0.1f * static_cast<float>(d), 0.0f),
                             .rotation = math::angleAxis(0.1f, math::vec3(0.0f, 1.0f, 0.0f))});
            parent = static_cast<int>(nodes.size()) - 1;
        }
    }
    return nodes;
}

template <class F>
double time_ms(int frames, F&& fn) {
    fn();  // warm-up
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        fn();
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
}

volatile double sink = 0.0;

// ---- ECS -----------------------------------------------------------------------------------------------------------

void bench_ecs(const char* label, const std::vector<Node>& nodes, int frames) {
    veach::World world;
    world.component<veach::Transform>();
    world.component<veach::GlobalTransform>();
    world.component<DTransform>();
    world.component<DGlobal>();
    world.component<FrameTransform>();
    std::vector<veach::Entity> ents;
    ents.reserve(nodes.size());
    for (const Node& n : nodes) {
        veach::Entity e = world.entity();
        if (n.parent >= 0) {
            e.child_of(ents[static_cast<std::size_t>(n.parent)]);
        }
        e.set<veach::Transform>({.translation = n.translation, .rotation = n.rotation});
        e.set<veach::GlobalTransform>({});
        e.set<DTransform>({.translation = math::dvec3(n.translation), .rotation = n.rotation});
        e.set<DGlobal>({});
        e.set<FrameTransform>({});
        ents.push_back(e);
    }

    // The engine's function builds a cached query per call and never frees it (it is entity-backed, so the
    // destructor leaves it to world cleanup); the engine's systems keep theirs. Time a few calls only: hundreds of
    // leaked cascade queries crash ecs_fini in release builds.
    const double fe = time_ms(2, [&] { veach::propagate_transforms(world); });
    auto q = world.query_builder<const veach::Transform, const veach::GlobalTransform*, veach::GlobalTransform>()
                 .term_at(1)
                 .parent()
                 .cascade()
                 .build();
    const double f = time_ms(frames, [&] {
        q.each([](const veach::Transform& local, const veach::GlobalTransform* parent, veach::GlobalTransform& global) {
            global.matrix = parent ? parent->matrix * local.matrix() : local.matrix();
        });
    });
    std::printf("ecs  %-22s propagate_transforms() (query built per call): %6.3f ms\n", label, fe);
    auto qd = world.query_builder<const DTransform, const DGlobal*, DGlobal>().term_at(1).parent().cascade().build();
    const double d = time_ms(frames, [&] { qd.each(compose_d); });
    auto qf = world.query_builder<const DTransform, const FrameTransform*, FrameTransform>()
                  .term_at(1)
                  .parent()
                  .cascade()
                  .build();
    const double fr = time_ms(frames, [&] { qf.each(compose_frame); });

    // The double results agree with the float ones.
    const auto* g = ents.back().try_get<veach::GlobalTransform>();
    const auto* gd = ents.back().try_get<DGlobal>();
    const auto* gf = ents.back().try_get<FrameTransform>();
    sink = sink + static_cast<double>(g->matrix[3].x) + gd->matrix[3].x + gf->translation.x;
    std::printf(
        "ecs  %-22s %6zu entities: float mat4 %6.3f ms | dmat4 %6.3f ms (x%.2f) | FrameTransform %6.3f ms "
        "(x%.2f)  [last x: %.6f / %.6f / %.6f]\n",
        label, nodes.size(), f, d, d / f, fr, fr / f, static_cast<double>(g->matrix[3].x), gd->matrix[3].x,
        gf->translation.x);
    std::fflush(stdout);
    q.destruct();
    qd.destruct();
    qf.destruct();
}

// ---- flat arrays ---------------------------------------------------------------------------------------------------

void bench_flat(const char* label, const std::vector<Node>& nodes, int frames) {
    const std::size_t n = nodes.size();
    std::vector<veach::Transform> local(n);
    std::vector<DTransform> dlocal(n);
    for (std::size_t i = 0; i < n; ++i) {
        local[i] = {.translation = nodes[i].translation, .rotation = nodes[i].rotation};
        dlocal[i] = {.translation = math::dvec3(nodes[i].translation), .rotation = nodes[i].rotation};
    }
    std::vector<math::mat4> gf(n);
    std::vector<math::dmat4> gd(n);
    std::vector<FrameTransform> gfr(n);

    const double f = time_ms(frames, [&] {
        for (std::size_t i = 0; i < n; ++i) {
            const int p = nodes[i].parent;
            gf[i] = p >= 0 ? gf[static_cast<std::size_t>(p)] * local[i].matrix() : local[i].matrix();
        }
    });
    const double d = time_ms(frames, [&] {
        for (std::size_t i = 0; i < n; ++i) {
            const int p = nodes[i].parent;
            gd[i] = p >= 0 ? gd[static_cast<std::size_t>(p)] * dmatrix_of(dlocal[i]) : dmatrix_of(dlocal[i]);
        }
    });
    const double fr = time_ms(frames, [&] {
        for (std::size_t i = 0; i < n; ++i) {
            const int p = nodes[i].parent;
            compose_frame(dlocal[i], p >= 0 ? &gfr[static_cast<std::size_t>(p)] : nullptr, gfr[i]);
        }
    });
    std::printf(
        "flat %-22s %6zu entities: float mat4 %6.3f ms | dmat4 %6.3f ms (x%.2f) | FrameTransform %6.3f ms "
        "(x%.2f)\n",
        label, n, f, d, d / f, fr, fr / f);

    // Extraction: 100k camera-relative float32 draw matrices.
    const math::dvec3 camera(1.496e11, 2.0e10, -3.0e9);
    std::vector<math::mat4> out(n);
    const double ef = time_ms(frames, [&] {  // today: copy the world matrix
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = gf[i];
        }
    });
    const double ed = time_ms(frames, [&] {  // dmat4: subtract the camera, cast
        for (std::size_t i = 0; i < n; ++i) {
            math::dmat4 m = gd[i];
            m[3] = math::dvec4(math::dvec3(m[3]) - camera, 1.0);
            out[i] = math::mat4(m);
        }
    });
    const double efr = time_ms(frames, [&] {  // FrameTransform: subtract the camera, cast the translation only
        for (std::size_t i = 0; i < n; ++i) {
            math::mat4 m(gfr[i].basis);
            m[3] = math::vec4(math::vec3(gfr[i].translation - camera), 1.0f);
            out[i] = m;
        }
    });
    sink = sink + static_cast<double>(out[n - 1][3].x);
    std::printf("extract %-19s %6zu draws:    float copy %6.3f ms | dmat4 %6.3f ms | FrameTransform %6.3f ms\n", label,
                n, ef, ed, efr);
}

}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 200;
    const std::vector<Node> wide = make_tree(1000, 9, 10);  // 100,000: depth 3
    const std::vector<Node> deep = make_chains(10000, 10);  // 100,000: depth 10
    bench_ecs("1k roots x 9 x 10", wide, frames);
    bench_ecs("10k chains x 10", deep, frames);
    bench_flat("1k roots x 9 x 10", wide, frames);
    bench_flat("10k chains x 10", deep, frames);
    return sink == 12345.0 ? 1 : 0;
}
