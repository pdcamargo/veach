// Spike SP-08 (VCH-084), throwaway: marching cubes + Transvoxel against surface nets + skirts on a 300 m sphere with a
// tunnel, in a restricted octree of 32³-cell chunks around a focus on the surface
// (docs/research/spikes/sp-08-mesher.md).
//
// Prints, per mesher: chunk and triangle counts per level, ms per chunk (sampling and meshing apart), the topological
// seam check, a ray-cast hole check over the seam rings, and mesh hashes on 1 and N threads. With --out it also writes
// a lab project (glTF of every chunk, colored by level, and one Luau script per camera) for screenshots:
//   build/release/tests/veach-bench-sp08-mesher --threads 8 --out build/sp08
//   build/release/apps/lab/veach-lab --project build/sp08/tv --rhi metal --headless --frames 3 \
//       --script scripts/seam.luau --screenshot docs/research/spikes/sp-08/tv_seam.png

#include "support/sp08_field.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <string>

namespace {

using namespace sp08;

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Options {
    unsigned threads = 8;
    double lambda = 4.0;
    int width_256 = 128;
    int skirt_256 = 256;
    int repeat = 3;
    std::string out;
    std::string only;
    bool flat = false;
    std::array<double, 6> look{};  ///< extra camera 'look': eye and target relative to the focus
    bool has_look = false;
    bool double_sided_special = false;  ///< transition cells and skirts marked double-sided (no effect today)
    bool world_positions = false;       ///< export focus-relative positions and zero node translations
    bool no_highlight = false;          ///< one primitive per chunk (transition cells and skirts not colored apart)
};

struct MesherCase {
    std::string id;
    std::function<std::unique_ptr<IsoMesher>()> make;
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    return v[i];
}

// ---- Ray-cast hole check: parallel rays over the seam rings; a ray that hits no triangle, or a back face first,
// looked through a crack (the body fills every ray's path).
struct RayReport {
    std::size_t rays = 0;          ///< rays that pass through the solid core, so must hit the surface
    std::size_t misses = 0;        ///< of those, rays that hit no triangle: a hole
    std::size_t back_first = 0;    ///< of those, rays whose first hit is a back face (a crack, or a skirt from behind)
    std::size_t through_rock = 0;  ///< of those, rays whose first front face lies behind rock: seen through a crack
    std::vector<std::array<double, 6>> cracks;  ///< first few: hit point relative to the focus, and ray direction
};

RayReport cast_rays(const SphereField& field, const std::vector<MeshedChunk>& chunks,
                    const std::array<double, 3>& focus_m, double half, double spacing,
                    const std::array<double, 3>& dir_in) {
    const double dl = std::sqrt(dir_in[0] * dir_in[0] + dir_in[1] * dir_in[1] + dir_in[2] * dir_in[2]);
    const std::array<double, 3> dir{dir_in[0] / dl, dir_in[1] / dl, dir_in[2] / dl};
    // Basis: u perpendicular to dir and z, v = dir × u.
    std::array<double, 3> u{dir[1], -dir[0], 0.0};
    if (std::abs(u[0]) + std::abs(u[1]) < 1e-9) {
        u = {1.0, 0.0, 0.0};
    }
    const double ul = std::sqrt(u[0] * u[0] + u[1] * u[1]);
    u = {u[0] / ul, u[1] / ul, 0.0};
    const std::array<double, 3> v{dir[1] * u[2] - dir[2] * u[1], dir[2] * u[0] - dir[0] * u[2],
                                  dir[0] * u[1] - dir[1] * u[0]};
    const auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    struct Tri {
        std::array<double, 3> p0, e1, e2, n;
    };
    std::vector<Tri> tris;
    const double bin = 0.5;
    const auto bins = static_cast<int>(std::ceil(2.0 * half / bin));
    std::vector<std::vector<std::uint32_t>> grid(static_cast<std::size_t>(bins * bins));
    for (const auto& c : chunks) {
        const ChunkMesh& m = c.mesh;
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            std::array<std::array<double, 3>, 3> p{};
            for (std::size_t k = 0; k < 3; ++k) {
                for (std::size_t a = 0; a < 3; ++a) {
                    p[k][a] =
                        static_cast<double>(m.fixed[m.indices[t + k]][a]) * (lod0_cell_meters / fixed_one) - focus_m[a];
                }
            }
            double umin = 1e30, umax = -1e30, vmin = 1e30, vmax = -1e30;
            for (const auto& q : p) {
                umin = std::min(umin, dot(q, u));
                umax = std::max(umax, dot(q, u));
                vmin = std::min(vmin, dot(q, v));
                vmax = std::max(vmax, dot(q, v));
            }
            if (umax < -half || umin > half || vmax < -half || vmin > half) {
                continue;
            }
            Tri tri{p[0], {}, {}, {}};
            for (std::size_t a = 0; a < 3; ++a) {
                tri.e1[a] = p[1][a] - p[0][a];
                tri.e2[a] = p[2][a] - p[0][a];
            }
            tri.n = {tri.e1[1] * tri.e2[2] - tri.e1[2] * tri.e2[1], tri.e1[2] * tri.e2[0] - tri.e1[0] * tri.e2[2],
                     tri.e1[0] * tri.e2[1] - tri.e1[1] * tri.e2[0]};
            const auto id = static_cast<std::uint32_t>(tris.size());
            tris.push_back(tri);
            const int i0 = std::max(0, static_cast<int>(std::floor((umin + half) / bin)));
            const int i1 = std::min(bins - 1, static_cast<int>(std::floor((umax + half) / bin)));
            const int j0 = std::max(0, static_cast<int>(std::floor((vmin + half) / bin)));
            const int j1 = std::min(bins - 1, static_cast<int>(std::floor((vmax + half) / bin)));
            for (int j = j0; j <= j1; ++j) {
                for (int i = i0; i <= i1; ++i) {
                    grid[static_cast<std::size_t>(i + bins * j)].push_back(id);
                }
            }
        }
    }
    RayReport r;
    const auto steps = static_cast<int>(2.0 * half / spacing);
    for (int j = 0; j < steps; ++j) {
        for (int i = 0; i < steps; ++i) {
            const double su = -half + (i + 0.5) * spacing, sv = -half + (j + 0.5) * spacing;
            const int bi = std::min(bins - 1, static_cast<int>((su + half) / bin));
            const int bj = std::min(bins - 1, static_cast<int>((sv + half) / bin));
            std::array<double, 3> o{};
            for (std::size_t a = 0; a < 3; ++a) {
                o[a] = su * u[a] + sv * v[a] - 2000.0 * dir[a];
            }
            double best = 1e30, front = 1e30;
            bool back = false;
            for (const std::uint32_t id : grid[static_cast<std::size_t>(bi + bins * bj)]) {
                const Tri& t = tris[id];
                // Möller-Trumbore, both sides.
                const std::array<double, 3> pv{dir[1] * t.e2[2] - dir[2] * t.e2[1], dir[2] * t.e2[0] - dir[0] * t.e2[2],
                                               dir[0] * t.e2[1] - dir[1] * t.e2[0]};
                const double det = dot(t.e1, pv);
                if (std::abs(det) < 1e-18) {
                    continue;
                }
                const double inv = 1.0 / det;
                const std::array<double, 3> tv{o[0] - t.p0[0], o[1] - t.p0[1], o[2] - t.p0[2]};
                const double bu = dot(tv, pv) * inv;
                if (bu < 0.0 || bu > 1.0) {
                    continue;
                }
                const std::array<double, 3> qv{tv[1] * t.e1[2] - tv[2] * t.e1[1], tv[2] * t.e1[0] - tv[0] * t.e1[2],
                                               tv[0] * t.e1[1] - tv[1] * t.e1[0]};
                const double bv = dot(dir, qv) * inv;
                if (bv < 0.0 || bu + bv > 1.0) {
                    continue;
                }
                const double dist = dot(t.e2, qv) * inv;
                const bool facing_away = dot(t.n, dir) > 0.0;
                if (dist > 0.0 && dist < best) {
                    best = dist;
                    back = facing_away;
                }
                if (dist > 0.0 && !facing_away && dist < front) {
                    front = dist;
                }
            }
            // Only rays whose line passes through the solid core (deeper than the relief, away from the tunnel) are
            // sure to meet the surface.
            const std::array<double, 3> co{-focus_m[0] - o[0], -focus_m[1] - o[1], -focus_m[2] - o[2]};
            const double along = dot(co, dir);
            const double closest2 = dot(co, co) - along * along;
            const double core = SphereField::radius - SphereField::relief - 1.0;
            if (closest2 > core * core) {
                continue;
            }
            ++r.rays;
            if (best == 1e30) {
                ++r.misses;
            } else if (back) {
                ++r.back_first;
                // With back faces culled the ray shows the first front face: a crack if the ray crossed rock to
                // reach it (or reached none).
                bool through_rock = front == 1e30;
                for (double t = best + 0.25; !through_rock && t < front; t += 0.25) {
                    through_rock = field.distance(focus_m[0] + o[0] + t * dir[0], focus_m[1] + o[1] + t * dir[1],
                                                  focus_m[2] + o[2] + t * dir[2]) < -0.25;
                }
                r.through_rock += through_rock;
                if (through_rock && r.cracks.size() < 4) {
                    // Where the ray enters the rock: the crack it slipped through.
                    double t = 1900.0;  // the origin is 2000 m before the focus plane
                    while (t < best && field.distance(focus_m[0] + o[0] + t * dir[0], focus_m[1] + o[1] + t * dir[1],
                                                      focus_m[2] + o[2] + t * dir[2]) >= 0.0) {
                        t += 0.02;
                    }
                    r.cracks.push_back(
                        {o[0] + t * dir[0], o[1] + t * dir[1], o[2] + t * dir[2], dir[0], dir[1], dir[2]});
                }
            }
        }
    }
    return r;
}

// ---- Lab project: glTF of every chunk (node at the chunk origin, relative to the focus), colored by level, with
// transition cells or skirts as a second, light primitive; and one Luau script per camera.
void write_project(const std::filesystem::path& dir, const std::vector<MeshedChunk>& chunks,
                   const std::array<double, 3>& focus_m, bool flat, bool highlight, const Options& opt) {
    std::filesystem::create_directories(dir / "assets");
    std::filesystem::create_directories(dir / "scripts");
    std::vector<char> bin;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* c = static_cast<const char*>(p);
        bin.insert(bin.end(), c, c + n);
    };
    std::string meshes, nodes, accessors, views, node_list;
    int accessor = 0, view = 0, mesh_id = 0;
    const auto add_view = [&](std::size_t offset, std::size_t length, int target) {
        views += std::format("{}{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{},\"target\":{}}}", view ? "," : "",
                             offset, length, target);
        return view++;
    };
    for (const auto& c : chunks) {
        const ChunkMesh& m = c.mesh;
        if (m.indices.empty()) {
            continue;
        }
        const std::size_t special = c.mesh.transition_triangles + c.mesh.skirt_triangles;
        const std::size_t total = m.triangle_count();
        std::vector<std::array<float, 3>> pos, nrm;
        std::vector<std::uint32_t> idx;
        if (flat) {
            for (std::size_t t = 0; t < total; ++t) {
                const auto& a = m.position[m.indices[3 * t]];
                const auto& b = m.position[m.indices[3 * t + 1]];
                const auto& cc = m.position[m.indices[3 * t + 2]];
                const std::array<float, 3> e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const std::array<float, 3> e2{cc[0] - a[0], cc[1] - a[1], cc[2] - a[2]};
                std::array<float, 3> n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                       e1[0] * e2[1] - e1[1] * e2[0]};
                const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (float& x : n) {
                    x = l > 0.0f ? x / l : 0.0f;
                }
                for (const auto* p : {&a, &b, &cc}) {
                    idx.push_back(static_cast<std::uint32_t>(pos.size()));
                    pos.push_back(*p);
                    nrm.push_back(n);
                }
            }
        } else {
            pos = m.position;
            nrm = m.normal;
            idx = m.indices;
        }
        const auto o = chunk_origin(m.key);
        if (opt.world_positions) {
            // Positions relative to the focus straight from the canonical fixed point, node translations zero: rules
            // out per-chunk rebasing as a source of pixel gaps.
            for (std::size_t v = 0; v < pos.size(); ++v) {
                const std::size_t src = flat ? m.indices[v] : v;
                for (std::size_t a = 0; a < 3; ++a) {
                    pos[v][a] = static_cast<float>(
                        static_cast<double>(m.fixed[src][a]) * (lod0_cell_meters / fixed_one) - focus_m[a]);
                }
            }
        }
        std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const auto& p : pos) {
            for (std::size_t a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }
        const std::size_t pos_off = bin.size();
        put(pos.data(), pos.size() * 12);
        const std::size_t nrm_off = bin.size();
        put(nrm.data(), nrm.size() * 12);
        const std::size_t idx_off = bin.size();
        put(idx.data(), idx.size() * 4);
        const int pv = add_view(pos_off, pos.size() * 12, 34962);
        const int nv = add_view(nrm_off, nrm.size() * 12, 34962);
        const int iv = add_view(idx_off, idx.size() * 4, 34963);
        const int pa = accessor++, na = accessor++;
        accessors += std::format(
            "{}{{\"bufferView\":{},\"componentType\":5126,\"count\":{},\"type\":\"VEC3\",\"min\":[{},{},{}],"
            "\"max\":[{},{},{}]}},{{\"bufferView\":{},\"componentType\":5126,\"count\":{},\"type\":\"VEC3\"}}",
            pa ? "," : "", pv, pos.size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], nv, nrm.size());
        const std::size_t regular = highlight ? total - special : total;
        std::string prims;
        const auto prim = [&](std::size_t first, std::size_t count, int material) {
            if (count == 0) {
                return;
            }
            const int ia = accessor++;
            accessors += std::format(
                ",{{\"bufferView\":{},\"byteOffset\":{},\"componentType\":5125,\"count\":{},"
                "\"type\":\"SCALAR\"}}",
                iv, first * 12, count * 3);
            prims +=
                std::format("{}{{\"attributes\":{{\"POSITION\":{},\"NORMAL\":{}}},\"indices\":{},\"material\":{}}}",
                            prims.empty() ? "" : ",", pa, na, ia, material);
        };
        prim(0, regular, c.leaf.key.level);
        prim(regular, total - regular, 6);
        meshes += std::format("{}{{\"primitives\":[{}]}}", mesh_id ? "," : "", prims);
        const double keep = opt.world_positions ? 0.0 : 1.0;
        nodes += std::format("{}{{\"mesh\":{},\"translation\":[{},{},{}]}}", mesh_id ? "," : "", mesh_id,
                             keep * (static_cast<double>(o[0]) * lod0_cell_meters - focus_m[0]),
                             keep * (static_cast<double>(o[1]) * lod0_cell_meters - focus_m[1]),
                             keep * (static_cast<double>(o[2]) * lod0_cell_meters - focus_m[2]));
        node_list += std::format("{}{}", mesh_id ? "," : "", mesh_id);
        ++mesh_id;
    }
    // Level colors (0 finest): distinct hues; 6: transition cells or skirts.
    constexpr std::array<std::array<float, 3>, 7> colors{{{0.85f, 0.30f, 0.25f},
                                                          {0.90f, 0.65f, 0.20f},
                                                          {0.35f, 0.70f, 0.30f},
                                                          {0.25f, 0.55f, 0.85f},
                                                          {0.55f, 0.35f, 0.80f},
                                                          {0.60f, 0.60f, 0.60f},
                                                          {0.95f, 0.95f, 0.95f}}};
    std::string materials;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        materials += std::format(
            "{}{{\"pbrMetallicRoughness\":{{\"baseColorFactor\":[{},{},{},1],\"metallicFactor\":0,"
            "\"roughnessFactor\":0.9}}{}}}",
            i ? "," : "", colors[i][0], colors[i][1], colors[i][2],
            (i == 6 && opt.double_sided_special) ? ",\"doubleSided\":true" : "");
    }
    std::ofstream(dir / "assets/terrain.bin", std::ios::binary)
        .write(bin.data(), static_cast<std::streamsize>(bin.size()));
    std::ofstream(dir / "assets/terrain.gltf") << std::format(
        "{{\"asset\":{{\"version\":\"2.0\",\"generator\":\"veach-bench-sp08-mesher\"}},\"scene\":0,"
        "\"scenes\":[{{\"nodes\":[{}]}}],\"nodes\":[{}],\"meshes\":[{}],\"materials\":[{}],"
        "\"accessors\":[{}],\"bufferViews\":[{}],\"buffers\":[{{\"uri\":\"terrain.bin\","
        "\"byteLength\":{}}}]}}\n",
        node_list, nodes, meshes, materials, accessors, views, bin.size());
    std::ofstream(dir / "veach.project.json")
        << "{\n  \"name\": \"sp08\",\n  \"script\": \"scripts/overview.luau\",\n  \"window\": { \"title\": \"SP-08\", "
           "\"width\": 1280, \"height\": 720 }\n}\n";

    // Cameras, relative to the focus (the top of the sphere, +Y up). Sky: constant magenta, so a crack that shows
    // the sky through the body is obvious.
    struct View {
        const char* name;
        std::array<double, 3> eye, target;
    };
    // The tunnel runs along x, so the seam views look at faces on +z, away from it. The ground at (x, z) lies about
    // (x * x + z * z) / 600 m below the focus.
    std::vector<View> cams{{
        {"overview", {0.0, 170.0, 150.0}, {0.0, -20.0, -20.0}},
        {"seam", {6.0, -1.0, 72.0}, {0.0, -8.0, 64.0}},         // LOD0 | LOD1 face at z = +64 m, from 7 m
        {"seam_low", {-14.0, -4.0, 64.0}, {12.0, -8.5, 64.0}},  // grazing along that face
        {"corner", {74.0, -6.0, 76.0}, {64.0, -14.0, 64.0}},    // the LOD0 square's corner: two faces and an edge
        {"rings", {12.0, -14.0, 142.0}, {0.0, -28.0, 128.0}},   // LOD1 | LOD2 face at z = +128 m
        {"cave", {72.0, 2.0, 10.0}, {101.0, -18.0, 0.0}},       // the tunnel's mouth on +x
    }};
    if (opt.has_look) {
        cams.push_back({"look", {opt.look[0], opt.look[1], opt.look[2]}, {opt.look[3], opt.look[4], opt.look[5]}});
    }
    for (const View& cam : cams) {
        std::ofstream(dir / std::format("scripts/{}.luau", cam.name)) << std::format(
            "--!strict\n-- Generated by veach-bench-sp08-mesher (spike SP-08): camera '{}'.\n"
            "local veach = require(\"@veach\")\nlocal c = veach.components\n"
            "veach.spawn(\"terrain\"):set(c.SceneInstance, {{ path = \"assets/terrain.gltf\" }})\n"
            "local eye = vector.create({}, {}, {})\nlocal target = vector.create({}, {}, {})\n"
            "local camera = veach.spawn(\"camera\")\n"
            "camera:set(c.Transform, {{ translation = veach.position(eye), rotation = veach.quat.look_rotation(target "
            "- eye), "
            "scale = vector.one }})\n"
            "camera:set(c.Camera, {{ fov_y = math.rad(60), near_plane = 0.1, active = true, ev100 = 12 }})\n"
            "camera:set(c.Sky, {{ mode = \"constant\", color = vector.create(600, 0, 600) }})\n"
            "local sun = veach.spawn(\"sun\")\n"
            "sun:set(c.Transform, {{ translation = veach.position(0, 0, 0), rotation = "
            "veach.quat.look_rotation(vector.create(-0.6, -0.55, -0.55)), scale = vector.one }})\n"
            "sun:set(c.DirectionalLight, {{ color = vector.create(1, 0.97, 0.92), illuminance = 40000 }})\n"
            "veach.system(\"Size\", \"Startup\", veach.query(c.Window), function(e, w)\n"
            "\te:set(c.Window, {{ title = w.title, width = 1280, height = 720, mode = w.mode, vsync = w.vsync }})\n"
            "end)\n",
            cam.name, cam.eye[0], cam.eye[1], cam.eye[2], cam.target[0], cam.target[1], cam.target[2]);
    }
}

int run(const Options& opt) {
    const SphereField field;
    const std::array<double, 3> focus_m{0.0, SphereField::radius, 0.0};
    LodSettings lod;
    lod.focus = {focus_m[0] / lod0_cell_meters, focus_m[1] / lod0_cell_meters, focus_m[2] / lod0_cell_meters};
    lod.lambda = opt.lambda;
    lod.root_level = 5;  // 512 m roots, 2 × 2 × 2 of them: the body fits in ±512 m
    const auto leaves = select_chunks(lod);
    std::array<int, 8> per_level{};
    int with_finer = 0;
    for (const auto& l : leaves) {
        ++per_level[l.key.level];
        with_finer += l.neighbors.finer != 0;
    }
    std::printf("octree: lambda %.1f, %zu leaves (", opt.lambda, leaves.size());
    for (int l = 0; l <= 5; ++l) {
        std::printf("%sL%d %d", l ? ", " : "", l, per_level[static_cast<std::size_t>(l)]);
    }
    std::printf("), %d with a finer neighbor\n\n", with_finer);

    const std::vector<MesherCase> cases{
        {"tv", [&] { return make_transvoxel_mesher({.transitions = true, .width_256 = opt.width_256}); }},
        {"tv0", [&] { return make_transvoxel_mesher({.transitions = true, .width_256 = 0}); }},
        {"mc", [] { return make_transvoxel_mesher({.transitions = false}); }},
        {"sn", [&] { return make_surface_nets_mesher({.skirts = true, .skirt_256 = opt.skirt_256}); }},
        {"sn0", [] { return make_surface_nets_mesher({.skirts = false}); }},
    };
    const std::array<std::int64_t, 3> region_lo{-1024, -1024, -1024}, region_hi{1024, 1024, 1024};
    int failures = 0;
    for (const auto& mc : cases) {
        if (!opt.only.empty() && ("," + opt.only + ",").find("," + mc.id + ",") == std::string::npos) {
            continue;
        }
        auto name_probe = mc.make();
        std::printf("## %s (%s)\n\n", std::string(name_probe->name()).c_str(), mc.id.c_str());
        // Timing: best of `repeat` single-thread runs per chunk; then hashes on 1 and N threads.
        std::vector<MeshedChunk> one;
        std::vector<double> sample_ms(leaves.size(), 1e30), mesh_ms(leaves.size(), 1e30);
        double wall1 = 1e30;
        for (int rep = 0; rep < opt.repeat; ++rep) {
            const double t0 = now_ms();
            one = mesh_all(field, leaves, mc.make, 1, now_ms);
            wall1 = std::min(wall1, now_ms() - t0);
            for (std::size_t i = 0; i < one.size(); ++i) {
                sample_ms[i] = std::min(sample_ms[i], one[i].sample_ms);
                mesh_ms[i] = std::min(mesh_ms[i], one[i].mesh_ms);
            }
        }
        const double t0 = now_ms();
        const auto many = mesh_all(field, leaves, mc.make, opt.threads, now_ms);
        const double wall_n = now_ms() - t0;
        const auto again = mesh_all(field, leaves, mc.make, 1, now_ms);
        const std::uint64_t h1 = meshes_hash(one), hn = meshes_hash(many), h1b = meshes_hash(again);

        std::printf(
            "| level | meshed chunks | triangles/chunk (mean) | max | vertices/chunk (mean) | transition or skirt "
            "share | sample ms/chunk "
            "(median) | mesh ms/chunk (median) | p95 |\n|---|---|---|---|---|---|---|---|---|\n");
        std::size_t all_tris = 0, all_special = 0, meshed = 0, all_verts = 0;
        std::vector<double> all_mesh;
        for (int l = 0; l <= 5; ++l) {
            std::vector<double> sm, mm;
            std::size_t tris = 0, special = 0, maxt = 0, n = 0, verts = 0;
            for (std::size_t i = 0; i < one.size(); ++i) {
                if (one[i].leaf.key.level != l || one[i].mesh.indices.empty()) {
                    continue;
                }
                ++n;
                tris += one[i].mesh.triangle_count();
                verts += one[i].mesh.fixed.size();
                maxt = std::max(maxt, one[i].mesh.triangle_count());
                special += one[i].mesh.transition_triangles + one[i].mesh.skirt_triangles;
                sm.push_back(sample_ms[i]);
                mm.push_back(mesh_ms[i]);
                all_mesh.push_back(mesh_ms[i]);
            }
            if (n == 0) {
                continue;
            }
            meshed += n;
            all_tris += tris;
            all_special += special;
            all_verts += verts;
            std::printf("| %d | %zu | %.0f | %zu | %.0f | %.1f %% | %.2f | %.3f | %.3f |\n", l, n,
                        static_cast<double>(tris) / static_cast<double>(n), maxt,
                        static_cast<double>(verts) / static_cast<double>(n),
                        100.0 * static_cast<double>(special) / static_cast<double>(tris), percentile(sm, 0.5),
                        percentile(mm, 0.5), percentile(mm, 0.95));
        }
        double mesh_sum = 0.0;
        for (const double m : all_mesh) {
            mesh_sum += m;
        }
        std::printf("| all | %zu | %.0f | | %.0f | %.1f %% | | %.3f (mean %.3f) | %.3f |\n\n", meshed,
                    static_cast<double>(all_tris) / static_cast<double>(meshed),
                    static_cast<double>(all_verts) / static_cast<double>(meshed),
                    100.0 * static_cast<double>(all_special) / static_cast<double>(all_tris), percentile(all_mesh, 0.5),
                    mesh_sum / static_cast<double>(all_mesh.size()), percentile(all_mesh, 0.95));
        std::printf("total: %zu triangles; wall (sample + mesh, %zu leaves): %.1f ms on 1 thread, %.1f ms on %u\n",
                    all_tris, leaves.size(), wall1, wall_n, opt.threads);
        std::printf("hash: 0x%016llx (1 thread), 0x%016llx (1 thread, again), 0x%016llx (%u threads): %s\n",
                    static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h1b),
                    static_cast<unsigned long long>(hn), opt.threads, (h1 == hn && h1 == h1b) ? "equal" : "DIFFERENT");
        failures += !(h1 == hn && h1 == h1b);

        const SeamReport seams = check_seams(one, region_lo, region_hi);
        std::printf(
            "seams: %zu open edges (%zu in chunks at a level change), %zu non-manifold, %zu triangles, "
            "%.3f %% wound out of the solid\n",
            seams.open_edges, seams.open_at_level_change, seams.nonmanifold_edges, seams.triangles,
            100.0 * static_cast<double>(seams.wound_out) / static_cast<double>(seams.triangles));
        const std::array<std::array<double, 3>, 5> dirs{
            {{0.0, -1.0, 0.0}, {0.7, -0.7, 0.0}, {-0.7, -0.7, 0.0}, {0.0, -0.7, 0.7}, {0.0, -0.7, -0.7}}};
        RayReport rays;
        for (const auto& d : dirs) {
            const RayReport r = cast_rays(field, one, focus_m, 96.0, 0.125, d);
            rays.rays += r.rays;
            rays.through_rock += r.through_rock;
            for (const auto& c : r.cracks) {
                if (rays.cracks.size() < 6) {
                    rays.cracks.push_back(c);
                }
            }
            rays.misses += r.misses;
            rays.back_first += r.back_first;
        }
        std::printf(
            "rays: %zu through the core, over ±96 m around the focus, 5 directions: %zu missed, %zu hit a back "
            "face first, %zu of them see through rock (cracks)\n",
            rays.rays, rays.misses, rays.back_first, rays.through_rock);
        for (const auto& c : rays.cracks) {
            std::printf("  crack seen at (%.2f, %.2f, %.2f) m from the focus, looking along (%.2f, %.2f, %.2f)\n", c[0],
                        c[1], c[2], c[3], c[4], c[5]);
        }

        // The ADR's "all six transition faces always": mesh cost with every face of every level > 0 chunk sampled
        // and meshed (finer mask 0x3F).
        if (mc.id == "tv") {
            auto mesher = mc.make();
            ChunkSamples samples;
            ChunkMesh mesh;
            std::vector<double> six;
            std::vector<double> six_sample;
            for (const auto& l : leaves) {
                if (l.key.level == 0 || !field.may_have_surface(l.key)) {
                    continue;
                }
                const ChunkNeighbors all{0x3F, 0};
                const double a = now_ms();
                sample_chunk(field, l.key, mesher->face_samples_needed(all), samples);
                const double b = now_ms();
                mesher->mesh(samples, all, mesh);
                const double c = now_ms();
                if (!mesh.indices.empty()) {
                    six.push_back(c - b);
                    six_sample.push_back(b - a);
                }
            }
            std::printf(
                "all six faces (levels 1+, %zu chunks): mesh ms/chunk median %.3f, p95 %.3f; sample ms/chunk "
                "median %.2f\n",
                six.size(), percentile(six, 0.5), percentile(six, 0.95), percentile(six_sample, 0.5));
        }
        std::printf("\n");
        if (!opt.out.empty()) {
            write_project(std::filesystem::path(opt.out) / mc.id, one, focus_m, opt.flat, !opt.no_highlight, opt);
        }
    }
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const auto arg = [&](const char* name) {
            return std::strcmp(argv[i], name) == 0 && i + 1 < argc;
        };
        if (arg("--threads")) {
            opt.threads = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg("--lambda")) {
            opt.lambda = std::strtod(argv[++i], nullptr);
        } else if (arg("--width")) {
            opt.width_256 = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (arg("--skirt")) {
            opt.skirt_256 = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (arg("--repeat")) {
            opt.repeat = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (arg("--out")) {
            opt.out = argv[++i];
        } else if (arg("--look")) {
            char* p = argv[++i];
            for (double& v : opt.look) {
                v = std::strtod(p, &p);
                p += (*p == ',');
            }
            opt.has_look = true;
        } else if (arg("--only")) {
            opt.only = argv[++i];
        } else if (std::strcmp(argv[i], "--flat") == 0) {
            opt.flat = true;
        } else if (std::strcmp(argv[i], "--no-highlight") == 0) {
            opt.no_highlight = true;
        } else if (std::strcmp(argv[i], "--world-positions") == 0) {
            opt.world_positions = true;
        } else if (std::strcmp(argv[i], "--double-sided") == 0) {
            opt.double_sided_special = true;
        } else {
            std::printf(
                "usage: veach-bench-sp08-mesher [--threads N] [--lambda L] [--width W/256] [--skirt S/256] [--repeat "
                "N] "
                "[--only tv,tv0,mc,sn,sn0] [--out DIR] [--flat] [--double-sided] [--world-positions] [--look "
                "ex,ey,ez,tx,ty,tz]\n");
            return 2;
        }
    }
    return run(opt);
}
