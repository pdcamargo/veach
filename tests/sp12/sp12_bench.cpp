// Spike SP-12 (VCH-088), throwaway: hashes, ms per chunk and the digging rate for the findings
// (docs/research/spikes/sp-12-dig-collide.md). Build it in Release for the timings.
//   veach-bench-sp12 [--half N] [--ops N] [--repeat N]
//       the scenario's hashes on 1 and 4 workers; sample, mesh and cook ms per chunk for a (2N + 1)³ region on 1 and
//       4 workers; then a trench dug one brush per 60 Hz tick (radius 1.5 m and 4 m), ms from op to cooked colliders
//   veach-bench-sp12 --find-site      bisects the field for the surface along (0.6, 0.8, 0): dig_site_mm
//   veach-bench-sp12 --out DIR        a lab project of the dug pit with the ball in it, for screenshots

#include "sp12/sp12_scenario.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace veach;
using namespace veach::sp12;
using voxel::BrushOp;

int parse_int(std::string_view s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// A collider cooked by engine/physics, with its cook time.
struct Cooked {
    std::uint32_t triangles = 0;
    double cook_ms = 0.0;
};
Cooked cook(const ChunkMesh& mesh) {
    const double t0 = now_ms();
    const auto c = physics::cook_collider(collider_mesh(mesh));
    return {c ? c->triangles : 0, now_ms() - t0};
}

struct Stats {
    double mean = 0.0, median = 0.0, p95 = 0.0, max = 0.0;
    std::size_t n = 0;
};
Stats stats(std::vector<double> v) {
    Stats s;
    if (v.empty()) {
        return s;
    }
    std::sort(v.begin(), v.end());
    s.n = v.size();
    for (const double x : v) {
        s.mean += x;
    }
    s.mean /= static_cast<double>(v.size());
    s.median = v[v.size() / 2];
    s.p95 = v[static_cast<std::size_t>(0.95 * static_cast<double>(v.size() - 1))];
    s.max = v.back();
    return s;
}
std::string fmt(const Stats& s) {
    return std::format("{:.3f} / {:.3f} / {:.3f} / {:.3f}", s.mean, s.median, s.p95, s.max);
}

int find_site() {
    const PlanetField field;
    const double ux = 0.6, uy = 0.8;
    double lo = PlanetField::radius - 20.0, hi = PlanetField::radius + 20.0;  // solid at lo, air at hi
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (field.distance(mid * ux, mid * uy, 0.0) < 0.0 ? lo : hi) = mid;
    }
    // Millimeters, rounded to a multiple of 5 along each axis of the 3-4-5 direction is not needed: the site is data.
    const auto x = static_cast<std::int64_t>(std::floor(lo * ux * 1000.0 + 0.5));
    const auto y = static_cast<std::int64_t>(std::floor(lo * uy * 1000.0 + 0.5));
    std::printf(
        "surface at r = %.6f m: dig_site_mm = {%lld, %lld, 0} (offsets from 3,822,600,000 and 5,096,800,000: "
        "%lld, %lld)\n",
        lo, static_cast<long long>(x), static_cast<long long>(y), static_cast<long long>(x - 3'822'600'000),
        static_cast<long long>(y - 5'096'800'000));
    return 0;
}

/// The surface height along radial up at (east, north) mm from the dig site, in meters (bisection on the field).
double surface_up(std::int64_t east, std::int64_t north) {
    const PlanetField field;
    const auto p = site_offset_mm(east, north, 0);
    const auto at = [&](double up_m) {
        return field.distance(static_cast<double>(p[0]) / 1000.0 + 0.6 * up_m,
                              static_cast<double>(p[1]) / 1000.0 + 0.8 * up_m, static_cast<double>(p[2]) / 1000.0);
    };
    double lo = -20.0, hi = 20.0;
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        (at(mid) < 0.0 ? lo : hi) = mid;
    }
    return lo;
}

int surface_height(std::int64_t east, std::int64_t north) {
    std::printf("surface at east %lld mm, north %lld mm: up %.0f mm\n", static_cast<long long>(east),
                static_cast<long long>(north), surface_up(east, north) * 1000.0);
    return 0;
}

/// The local minima of the surface within 16 m of the dig site, on a 0.5 m grid: where a ball comes to rest.
int find_basins() {
    constexpr int n = 32;
    std::vector<double> h((2 * n + 1) * (2 * n + 1));
    const auto at = [&](int i, int j) -> double& {
        return h[static_cast<std::size_t>((i + n) + (2 * n + 1) * (j + n))];
    };
    for (int j = -n; j <= n; ++j) {
        for (int i = -n; i <= n; ++i) {
            at(i, j) = surface_up(500 * i, 500 * j);
        }
    }
    for (int j = -n + 2; j <= n - 2; ++j) {
        for (int i = -n + 2; i <= n - 2; ++i) {
            bool minimum = true;
            for (int dj = -2; dj <= 2; ++dj) {
                for (int di = -2; di <= 2; ++di) {
                    minimum = minimum && ((di == 0 && dj == 0) || at(i + di, j + dj) > at(i, j));
                }
            }
            if (minimum) {
                std::printf("basin at east %d mm, north %d mm: up %.0f mm\n", 500 * i, 500 * j, at(i, j) * 1000.0);
            }
        }
    }
    return 0;
}

void print_scenario(const char* label, const ScenarioResult& r) {
    const auto h = [](std::uint64_t v) {
        return std::format("0x{:016x}", v);
    };
    std::printf("%s\n    base samples %s mesh %s | edited samples %s mesh %s colliders %s install %s\n", label,
                h(r.base_samples_hash).c_str(), h(r.base_mesh_hash).c_str(), h(r.samples_hash).c_str(),
                h(r.mesh_hash).c_str(), h(r.colliders_hash).c_str(), h(r.install_hash).c_str());
    std::string balls;
    for (const int step : snapshot_steps) {
        if (static_cast<std::size_t>(step) <= r.step_hashes.size()) {
            balls += std::format(" {}: {}", step, h(r.step_hashes[static_cast<std::size_t>(step) - 1]));
        }
    }
    std::printf("    ball after step%s; chain %s jolt %s\n", balls.c_str(), h(r.chain).c_str(),
                h(r.jolt_state).c_str());
    std::printf(
        "    sampled %zu meshed %zu changed %zu materialized %zu outside %zu mismatches %zu bodies %zu | %zu "
        "triangles (%zu wound out), %zu open edges\n",
        r.sampled, r.meshed, r.changed, r.materialized, r.outside, r.mismatches, r.terrain_bodies, r.triangles,
        r.wound_out, r.open_edges);
    for (std::size_t b = 0; b < r.batches.size(); ++b) {
        std::printf("    batch %zu: %zu chunks; ball before it %.3f m deep, %s\n", b + 1, r.batches[b].chunks,
                    r.batches[b].depth_before, r.batches[b].asleep_before ? "asleep" : "awake");
    }
    std::printf("    ball at the end: %.3f m deep, %.3f m off the axis, %.4f m/s\n", r.depth, r.lateral, r.speed);
}

/// Sample, mesh and cook every chunk of a (2 half + 1)³ region on `workers` threads.
void load_timings(int half, std::uint32_t workers, int repeat) {
    std::vector<double> sample, mesh, cook_ms, load_wall, cook_wall;
    std::size_t sampled = 0, meshed = 0, triangles = 0;
    for (int rep = 0; rep < repeat; ++rep) {
        JobSystem jobs(workers);
        const PlanetField field;
        Terrain terrain(field, chunk_of_mm(dig_site_mm), half);
        const double t0 = now_ms();
        terrain.load(jobs);
        load_wall.push_back(now_ms() - t0);
        std::vector<const Chunk*> chunks;
        for (const Chunk& c : terrain.chunks()) {
            if (c.sampled) {
                sample.push_back(c.times.sample);
                mesh.push_back(c.times.mesh);
                if (!c.mesh.indices.empty()) {
                    chunks.push_back(&c);
                }
            }
        }
        std::vector<Cooked> colliders(chunks.size());
        const double t1 = now_ms();
        jobs.parallel_for("cook", chunks.size(), 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) {
                colliders[i] = cook(chunks[i]->mesh);
            }
        });
        cook_wall.push_back(now_ms() - t1);
        triangles = 0;
        for (const Cooked& c : colliders) {
            cook_ms.push_back(c.cook_ms);
            triangles += c.triangles;
        }
        sampled = terrain.sampled_count();
        meshed = chunks.size();
    }
    std::printf("| %u | %zu | %zu | %zu | %s | %s | %s | %.1f | %.1f |\n", workers, sampled, meshed,
                meshed ? triangles / meshed : 0, fmt(stats(sample)).c_str(), fmt(stats(mesh)).c_str(),
                fmt(stats(cook_ms)).c_str(), stats(load_wall).median, stats(cook_wall).median);
}

/// A trench dug one brush per tick: each op moves `step_mm` east, 0.5 m under the surface. Per op: apply + remesh the
/// changed chunks, then cook their colliders, on `workers` threads; the latency is op to every collider cooked.
void dig_rate(int half, std::uint32_t workers, std::int32_t radius_mm, std::int64_t step_mm, int ops) {
    JobSystem jobs(workers);
    const PlanetField field;
    Terrain terrain(field, chunk_of_mm(dig_site_mm), half);
    terrain.load(jobs);
    std::vector<double> latency, apply_ms, cook_ms, per_chunk_edit, per_chunk_mesh, per_chunk_cook, chunks_per_op;
    for (int i = 0; i < ops; ++i) {
        BrushOp op;
        op.kind = voxel::BrushKind::subtract;
        op.radius_mm = radius_mm;
        op.center_mm = site_offset_mm(-6'000 + step_mm * i, 0, -500);
        op.seq = static_cast<std::uint64_t>(i + 1);
        op.tick = static_cast<std::uint64_t>(i);
        const double t0 = now_ms();
        const std::vector<voxel::ChunkKey> changed = terrain.apply(op, jobs);
        const double t1 = now_ms();
        std::vector<Cooked> colliders(changed.size());
        jobs.parallel_for("cook", changed.size(), 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t k = b; k < e; ++k) {
                colliders[k] = cook(terrain.find(changed[k])->mesh);
            }
        });
        const double t2 = now_ms();
        latency.push_back(t2 - t0);
        apply_ms.push_back(t1 - t0);
        cook_ms.push_back(t2 - t1);
        chunks_per_op.push_back(static_cast<double>(changed.size()));
        for (std::size_t k = 0; k < changed.size(); ++k) {
            const Chunk* c = terrain.find(changed[k]);
            per_chunk_edit.push_back(c->times.edit);
            per_chunk_mesh.push_back(c->times.mesh);
            per_chunk_cook.push_back(colliders[k].cook_ms);
        }
    }
    const Stats l = stats(latency);
    const Stats cpo = stats(chunks_per_op);
    // At rest: 3 bytes per overridden sample (before RLE and zstd), against about 49 bytes per op in the journal.
    const std::size_t overrides = terrain.override_samples();
    std::printf("| %.1f | %u | %.2f / %.0f | %s | %s | %s | %s | %s | %zu samples, %.0f KB vs %.1f KB of ops |\n",
                radius_mm / 1000.0, workers, cpo.mean, cpo.max, fmt(stats(per_chunk_edit)).c_str(),
                fmt(stats(per_chunk_mesh)).c_str(), fmt(stats(per_chunk_cook)).c_str(), fmt(l).c_str(),
                std::format("{:.0f} %", 100.0 * l.mean / (1000.0 / 60.0)).c_str(), overrides,
                3.0 * static_cast<double>(overrides) / 1024.0, 49.0 * ops / 1024.0);
}

// ---- Lab project: the region's meshes and the ball, rotated so radial up is +Y (visual only), and a camera.
void write_project(const std::filesystem::path& dir, const ScenarioResult& r, bool one_material) {
    std::filesystem::create_directories(dir / "assets");
    std::filesystem::create_directories(dir / "scripts");
    // Rotation about z taking up (0.6, 0.8, 0) to +Y: x' = 0.8 x − 0.6 y, y' = 0.6 x + 0.8 y.
    const auto rotate = [](double x, double y, double z) {
        return std::array<float, 3>{static_cast<float>(0.8 * x - 0.6 * y), static_cast<float>(0.6 * x + 0.8 * y),
                                    static_cast<float>(z)};
    };
    std::vector<std::array<float, 3>> pos, nrm;
    std::vector<std::uint32_t> idx[4];  // per material: regolith, rock, mound, ball
    for (const auto& m : r.meshes) {
        const auto o = voxel::chunk_origin(m.key);
        const auto base = static_cast<std::uint32_t>(pos.size());
        for (std::size_t v = 0; v < m.position.size(); ++v) {
            double p[3];
            for (std::size_t a = 0; a < 3; ++a) {
                p[a] = static_cast<double>(o[a] - r.origin[a]) * voxel::lod0_cell_meters + m.position[v][a] - r.site[a];
            }
            pos.push_back(rotate(p[0], p[1], p[2]));
            nrm.push_back(rotate(m.normal[v][0], m.normal[v][1], m.normal[v][2]));
        }
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const std::uint8_t mat = m.material[m.indices[t]];
            auto& list = idx[one_material ? 0 : (mat == 3 ? 2 : (mat == 2 ? 1 : 0))];
            for (std::size_t k = 0; k < 3; ++k) {
                list.push_back(base + m.indices[t + k]);
            }
        }
    }
    // The ball: a UV sphere of radius 0.4 m.
    {
        const auto base = static_cast<std::uint32_t>(pos.size());
        const auto c = rotate(r.ball[0] - r.site[0], r.ball[1] - r.site[1], r.ball[2] - r.site[2]);
        constexpr int rings = 16, segments = 32;
        for (int i = 0; i <= rings; ++i) {
            const double th = 3.14159265358979 * i / rings;
            for (int j = 0; j <= segments; ++j) {
                const double ph = 2.0 * 3.14159265358979 * j / segments;
                const std::array<float, 3> n{static_cast<float>(std::sin(th) * std::cos(ph)),
                                             static_cast<float>(std::cos(th)),
                                             static_cast<float>(std::sin(th) * std::sin(ph))};
                pos.push_back({c[0] + 0.4f * n[0], c[1] + 0.4f * n[1], c[2] + 0.4f * n[2]});
                nrm.push_back(n);
            }
        }
        for (int i = 0; i < rings; ++i) {
            for (int j = 0; j < segments; ++j) {
                const auto a = base + static_cast<std::uint32_t>(i * (segments + 1) + j);
                const auto b = a + segments + 1;
                for (const std::uint32_t v : {a, a + 1, b, a + 1, b + 1, b}) {
                    idx[3].push_back(v);
                }
            }
        }
    }
    std::vector<char> bin;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* ch = static_cast<const char*>(p);
        bin.insert(bin.end(), ch, ch + n);
    };
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& p : pos) {
        for (std::size_t a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    put(pos.data(), pos.size() * 12);
    put(nrm.data(), nrm.size() * 12);
    std::string views = std::format(
        "{{\"buffer\":0,\"byteOffset\":0,\"byteLength\":{},\"target\":34962}},"
        "{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{},\"target\":34962}}",
        pos.size() * 12, pos.size() * 12, nrm.size() * 12);
    std::string accessors = std::format(
        "{{\"bufferView\":0,\"componentType\":5126,\"count\":{},\"type\":\"VEC3\",\"min\":[{},{},{}],\"max\":[{},{},{}]"
        "}},"
        "{{\"bufferView\":1,\"componentType\":5126,\"count\":{},\"type\":\"VEC3\"}}",
        pos.size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], nrm.size());
    std::string prims;
    int accessor = 2, view = 2;
    for (int m = 0; m < 4; ++m) {
        if (idx[m].empty()) {
            continue;
        }
        const std::size_t off = bin.size();
        put(idx[m].data(), idx[m].size() * 4);
        views += std::format(",{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{},\"target\":34963}}", off,
                             idx[m].size() * 4);
        accessors += std::format(",{{\"bufferView\":{},\"componentType\":5125,\"count\":{},\"type\":\"SCALAR\"}}",
                                 view++, idx[m].size());
        prims += std::format("{}{{\"attributes\":{{\"POSITION\":0,\"NORMAL\":1}},\"indices\":{},\"material\":{}}}",
                             prims.empty() ? "" : ",", accessor++, m);
    }
    std::ofstream(dir / "assets/terrain.bin", std::ios::binary)
        .write(bin.data(), static_cast<std::streamsize>(bin.size()));
    std::ofstream(dir / "assets/terrain.gltf") << std::format(
        "{{\"asset\":{{\"version\":\"2.0\",\"generator\":\"veach-bench-sp12\"}},\"scene\":0,"
        "\"scenes\":[{{\"nodes\":[0]}}],\"nodes\":[{{\"mesh\":0}}],\"meshes\":[{{\"primitives\":[{}]}}],"
        "\"materials\":["
        "{{\"pbrMetallicRoughness\":{{\"baseColorFactor\":[0.55,0.45,0.32,1],\"metallicFactor\":0,\"roughnessFactor\":"
        "0.9}}}},"
        "{{\"pbrMetallicRoughness\":{{\"baseColorFactor\":[0.40,0.40,0.42,1],\"metallicFactor\":0,\"roughnessFactor\":"
        "0.9}}}},"
        "{{\"pbrMetallicRoughness\":{{\"baseColorFactor\":[0.30,0.55,0.30,1],\"metallicFactor\":0,\"roughnessFactor\":"
        "0.9}}}},"
        "{{\"pbrMetallicRoughness\":{{\"baseColorFactor\":[0.85,0.15,0.10,1],\"metallicFactor\":0,\"roughnessFactor\":"
        "0.4}}}}],"
        "\"accessors\":[{}],\"bufferViews\":[{}],\"buffers\":[{{\"uri\":\"terrain.bin\",\"byteLength\":{}}}]}}\n",
        prims, accessors, views, bin.size());
    std::ofstream(dir / "veach.project.json")
        << "{\n  \"name\": \"sp12\",\n  \"script\": \"scripts/pit.luau\",\n  \"window\": { \"title\": \"SP-12\", "
           "\"width\": 1280, \"height\": 720 }\n}\n";
    struct View {
        const char* name;
        std::array<double, 3> eye, target;
    };
    const View cams[] = {{"pit", {1.5, 6.5, 3.0}, {0.0, -3.8, 0.0}},
                         {"side", {3.5, 4.0, 5.5}, {-0.3, -2.0, 0.0}},
                         {"overview", {14.0, 12.0, 16.0}, {-1.0, -2.0, 0.0}}};
    for (const View& cam : cams) {
        std::ofstream(dir / std::format("scripts/{}.luau", cam.name)) << std::format(
            "--!strict\n-- Generated by veach-bench-sp12 (spike SP-12): camera '{}'. +Y is radial up at the dig site.\n"
            "local veach = require(\"@veach\")\nlocal c = veach.components\n"
            "veach.spawn(\"terrain\"):set(c.SceneInstance, {{ path = \"assets/terrain.gltf\" }})\n"
            "local eye = vector.create({}, {}, {})\nlocal target = vector.create({}, {}, {})\n"
            "local camera = veach.spawn(\"camera\")\n"
            "camera:set(c.Transform, {{ translation = veach.position(eye), rotation = veach.quat.look_rotation(target "
            "- eye), "
            "scale = vector.one }})\n"
            "camera:set(c.Camera, {{ fov_y = math.rad(60), near_plane = 0.1, active = true, ev100 = 13 }})\n"
            "local sun = veach.spawn(\"sun\")\n"
            "sun:set(c.Transform, {{ translation = veach.position(0, 0, 0), rotation = "
            "veach.quat.look_rotation(vector.create(-0.3, -1, -0.45)), scale = vector.one }})\n"
            "sun:set(c.DirectionalLight, {{ color = vector.create(1, 0.97, 0.92), illuminance = 40000 }})\n"
            "veach.system(\"Size\", \"Startup\", veach.query(c.Window), function(e, w)\n"
            "\te:set(c.Window, {{ title = w.title, width = 1280, height = 720, mode = w.mode, vsync = w.vsync }})\n"
            "end)\n",
            cam.name, cam.eye[0], cam.eye[1], cam.eye[2], cam.target[0], cam.target[1], cam.target[2]);
    }
    std::printf("wrote %s (%zu vertices)\n", dir.string().c_str(), pos.size());
}

}  // namespace

int main(int argc, char** argv) {
    int half = 3, ops = 240, repeat = 3, steps = 1200;
    std::string out;
    bool one_material = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--find-site") {
            return find_site();
        } else if (a == "--basins") {
            return find_basins();
        } else if (a == "--surface" && i + 2 < argc) {
            return surface_height(parse_int(argv[i + 1]), parse_int(argv[i + 2]));
        } else if (a == "--half" && more) {
            half = parse_int(argv[++i]);
        } else if (a == "--ops" && more) {
            ops = parse_int(argv[++i]);
        } else if (a == "--repeat" && more) {
            repeat = parse_int(argv[++i]);
        } else if (a == "--steps" && more) {
            steps = parse_int(argv[++i]);
        } else if (a == "--one-material") {
            one_material = true;
        } else if (a == "--out" && more) {
            out = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: veach-bench-sp12 [--find-site] [--half N] [--ops N] [--repeat N] "
                         "[--out DIR]\n");
            return 2;
        }
    }
    if (!out.empty()) {
        ScenarioConfig config;
        config.keep_meshes = true;
        config.steps = steps;
        const ScenarioResult r = run_scenario(config);
        print_scenario("scenario", r);
        write_project(out, r, one_material);
        return 0;
    }
    for (const std::uint32_t w : {1u, 4u}) {
        ScenarioConfig config;
        config.workers = w;
        const double t0 = now_ms();
        const ScenarioResult r = run_scenario(config);
        print_scenario(std::format("scenario, {} worker(s), {:.0f} ms", w, now_ms() - t0).c_str(), r);
    }
    std::printf("\nRegion of %d^3 chunks, %s; ms per chunk as mean / median / p95 / max; wall ms median of %d\n\n",
                2 * half + 1, "FavorRuntimePerformance", repeat);
    std::printf(
        "| Workers | Sampled | Meshed | Triangles per chunk | Sample ms | Mesh ms | Cook ms | Load wall ms | "
        "Cook wall ms |\n|---|---|---|---|---|---|---|---|---|\n");
    for (const std::uint32_t w : {1u, 4u}) {
        load_timings(half, w, repeat);
    }
    std::printf(
        "\nTrench, %d ops (30 for the 16 m brush), one per tick, moving 50 mm per op, 0.5 m under the surface; per "
        "changed chunk ms as mean / median / p95 / max\n\n",
        ops);
    std::printf(
        "| Radius m | Workers | Chunks per op (mean / max) | Edit ms | Mesh ms | Cook ms | Op to colliders ms | "
        "Mean share of a 16.7 ms tick | Overrides at rest |\n|---|---|---|---|---|---|---|---|---|\n");
    for (const std::int32_t radius : {1'500, 4'000, voxel::max_brush_radius_mm}) {
        for (const std::uint32_t w : {1u, 4u}) {
            dig_rate(half, w, radius, 50, radius > 4'000 ? std::min(ops, 30) : ops);
        }
    }
    return 0;
}
