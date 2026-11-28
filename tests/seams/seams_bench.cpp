// Seams sweep, throwaway: the per-site tables of docs/research/spikes/seams-sweep.md.
//   veach-bench-seams lod      the multi-level octree around each site: levels, weld, cross-plane samples
//   veach-bench-seams dig      SP-12's dig-and-collide flow at each site
//   veach-bench-seams walk     the walks (add --legs N for longer runs)
//   veach-bench-seams cubemap  SP-07's climate cube map and SP-06's face regions across face edges
//   veach-bench-seams pole     SP-13's landing with the pad moved toward the north pole
//   veach-bench-seams trace --out DIR    per-step |Δv| of every walk in each collider variant, as CSV
//   veach-bench-seams shots --out DIR    lab projects for the screenshots in docs/research/spikes/seams/
//   veach-bench-seams lod --root-level N; walk --legs N

#include "seams/seams.hpp"
#include "seams/seams_cubemap.hpp"
#include "sp13/sp13_sim.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <veach/core/jobs.hpp>

namespace {

using namespace veach;
using namespace veach::seams;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* const variant_names[] = {"chunks", "merged", "edges", "ring", "adjacency"};

void lod(int root_level) {
    std::printf("%-12s %6s %7s %6s  %-28s %9s %5s %5s %6s %6s %8s %6s\n", "site", "leaves", "sampled", "meshed",
                "meshed per level", "triangles", "open", "nonmf", "bound", "trans", "shared", "mism");
    for (const SiteSpec& spec : sweep_sites()) {
        LodConfig c;
        c.root_level = root_level;
        c.site = make_site(spec);
        const double t0 = now_s();
        const LodResult r = run_lod(c);
        std::string levels;
        for (std::size_t l = 0; l < r.meshed_per_level.size(); ++l) {
            if (r.meshed_per_level[l] != 0) {
                levels += std::to_string(r.meshed_per_level[l]) + "@L" + std::to_string(l) + " ";
            }
        }
        std::printf("%-12s %6zu %7zu %6zu  %-28s %9zu %5zu %5zu %6zu %6zu %8zu %6zu  %.2f s  hash 0x%016llx\n",
                    std::string(spec.name).c_str(), r.leaves, r.sampled, r.meshed, levels.c_str(), r.seams.triangles,
                    r.seams.open_edges, r.seams.nonmanifold_edges, r.seams.boundary_edges, r.transition_triangles,
                    r.shared_points, r.shared_mismatches, now_s() - t0, static_cast<unsigned long long>(r.mesh_hash));
        const unsigned planes = octant_planes(c.site);
        for (int a = 0; a < 3; ++a) {
            if ((planes >> a & 1u) == 0) {
                continue;
            }
            const Continuity k = check_continuity(c.site, a);
            std::printf(
                "    plane %c = 0: %zu lines, max step across %.4f m, within %.4f m (bound %.3f), |d(+1um) - "
                "d(-1um)| max %.3g m\n",
                "xyz"[a], k.lines, k.max_step_across, k.max_step_within, k.lipschitz_bound, k.max_jump);
        }
    }
}

void dig() {
    std::printf("%-12s %7s %6s %7s %5s %5s %6s %9s %6s | %8s %8s %6s %6s %8s\n", "site", "sampled", "meshed", "changed",
                "mat", "mism", "open", "triangles", "bodies", "pit", "sump", "depth", "lateral", "speed");
    std::vector<SiteSpec> sites = sweep_sites();
    sites.push_back(control_site());
    for (const SiteSpec& spec : sites) {
        sp12::ScenarioConfig c;
        c.site = spec.name == "sp12" ? sp12::default_site : make_site(spec);  // the control reproduces SP-12
        c.workers = 0;
        const double t0 = now_s();
        const sp12::ScenarioResult r = sp12::run_scenario(c);
        std::printf(
            "%-12s %7zu %6zu %7zu %5zu %5zu %6zu %9zu %6zu | %8.3f %8.3f %6.3f %6.3f %8.4f %s  %.2f s\n"
            "    samples 0x%016llx mesh 0x%016llx colliders 0x%016llx install 0x%016llx chain 0x%016llx\n",
            std::string(spec.name).c_str(), r.sampled, r.meshed, r.changed, r.materialized, r.mismatches, r.open_edges,
            r.triangles, r.terrain_bodies, r.batches[0].depth_before, r.batches[1].depth_before, r.depth, r.lateral,
            r.speed, r.batches[1].asleep_before ? "asleep" : "awake", now_s() - t0,
            static_cast<unsigned long long>(r.samples_hash), static_cast<unsigned long long>(r.mesh_hash),
            static_cast<unsigned long long>(r.colliders_hash), static_cast<unsigned long long>(r.install_hash),
            static_cast<unsigned long long>(r.chain));
    }
}

void walk(int legs) {
    for (WalkConfig w : sweep_walks()) {
        w.legs = legs;
        for (const int variant : {0, 1, 2, 3, 4}) {
            w.merged = variant == 1;
            w.edge_removal = variant == 2;
            w.ring = variant == 3 ? Ring::duplicate : (variant == 4 ? Ring::adjacency : Ring::none);
            const double t0 = now_s();
            const WalkResult r = run_walk(w);
            std::printf(
                "%-38s %s: %zu colliders, %.1f m, %zu steps (%zu near chunk boundaries, %zu near octant planes), %zu "
                "chunk crossings, %zu octant crossings  %.2f s\n"
                "    unsupported %zu (max gap %.4f m), feet height %.4f..%.4f m, |dv| at boundaries max %.4f p99 %.4f, "
                "at planes max %.4f, interior max %.4f p99 %.4f m/s\n"
                "    up error max %.3g rad, heading step max %.3g rad, compass jump %.3f rad, farthest from the "
                "midpoint %.2f m, back at start within "
                "%.3f m, chain 0x%016llx\n",
                std::string(w.name).c_str(), variant_names[variant], r.colliders, r.distance, r.steps, r.boundary_steps,
                r.plane_steps, r.chunk_crossings, r.octant_crossings, now_s() - t0, r.unsupported,
                r.max_unsupported_gap, r.min_gap, r.max_gap, r.max_dv_boundary, r.p99_dv_boundary, r.max_dv_plane,
                r.max_dv_interior, r.p99_dv_interior, r.max_up_error, r.max_heading_step, r.compass_jump,
                r.max_from_mid, r.end_error, static_cast<unsigned long long>(r.chain));
            if (r.first_loss_leg >= 0) {
                std::printf(
                    "    first loss: leg %d step %d, ground state %d, feet (%.3f, %.3f, %.3f) m, %.4f m above "
                    "the colliders\n",
                    r.first_loss_leg, r.first_loss_step, r.first_loss_state, r.first_loss_feet[0], r.first_loss_feet[1],
                    r.first_loss_feet[2], r.first_loss_gap);
            }
        }
    }
}

std::array<double, 3> normalize3(const std::array<double, 3>& v) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / l, v[1] / l, v[2] / l};
}

// ---- Lab projects for screenshots: chunk meshes colored by a key function, a ball and a character, in a view frame
// where the site's east is +X, up +Y and north −Z (visual only).

struct Scene {
    std::array<double, 3> origin{};  ///< body-local meters: the view frame's origin
    std::array<double, 3> east{}, up{}, north{};
    std::vector<std::array<float, 3>> pos, nrm;
    std::vector<std::vector<std::uint32_t>> idx;  ///< per color
    std::vector<std::array<float, 3>> colors;

    std::array<float, 3> view(const std::array<double, 3>& v) const {
        return {static_cast<float>(v[0] * east[0] + v[1] * east[1] + v[2] * east[2]),
                static_cast<float>(v[0] * up[0] + v[1] * up[1] + v[2] * up[2]),
                static_cast<float>(-(v[0] * north[0] + v[1] * north[1] + v[2] * north[2]))};
    }
    std::array<float, 3> place(const std::array<double, 3>& body) const {
        return view({body[0] - origin[0], body[1] - origin[1], body[2] - origin[2]});
    }
    std::vector<std::uint32_t>& list(std::size_t color) {
        if (idx.size() <= color) {
            idx.resize(color + 1);
        }
        return idx[color];
    }
    /// A chunk mesh; `color` picks each chunk's color.
    void add_mesh(const voxel::ChunkMesh& m, std::size_t color) {
        const auto o = voxel::chunk_origin(m.key);
        const auto base = static_cast<std::uint32_t>(pos.size());
        for (std::size_t v = 0; v < m.position.size(); ++v) {
            std::array<double, 3> p{};
            for (std::size_t a = 0; a < 3; ++a) {
                p[a] = static_cast<double>(o[a]) * voxel::lod0_cell_meters + m.position[v][a];
            }
            pos.push_back(place(p));
            nrm.push_back(view({m.normal[v][0], m.normal[v][1], m.normal[v][2]}));
        }
        auto& l = list(color);
        for (const std::uint32_t i : m.indices) {
            l.push_back(base + i);
        }
    }
    /// A capsule (or a sphere when half_height is 0) standing on `feet` along `axis`.
    void add_capsule(const std::array<double, 3>& feet, const std::array<double, 3>& axis, double radius,
                     double half_height, std::size_t color) {
        const auto f = place(feet);
        const auto ax = view(axis);
        const auto base = static_cast<std::uint32_t>(pos.size());
        constexpr int rings = 16, segments = 32;
        // A UV sphere split at its equator and stretched by 2 half_height along the axis (in the view frame the axis is
        // close to +Y; the capsule is built along +Y, then tilted).
        for (int i = 0; i <= rings; ++i) {
            const double th = 3.14159265358979 * i / rings;
            const double lift = (i <= rings / 2 ? 2.0 * half_height : 0.0) + radius;
            for (int j = 0; j <= segments; ++j) {
                const double ph = 2.0 * 3.14159265358979 * j / segments;
                const std::array<double, 3> n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
                const std::array<double, 3> local{radius * n[0], radius * n[1] + lift, radius * n[2]};
                // Tilt +Y onto ax: for these views the tilt is under a degree, so a shear is enough.
                pos.push_back({f[0] + static_cast<float>(local[0] + local[1] * ax[0]),
                               f[1] + static_cast<float>(local[1] * ax[1]),
                               f[2] + static_cast<float>(local[2] + local[1] * ax[2])});
                nrm.push_back({static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2])});
            }
        }
        auto& l = list(color);
        for (int i = 0; i < rings; ++i) {
            for (int j = 0; j < segments; ++j) {
                const auto a = base + static_cast<std::uint32_t>(i * (segments + 1) + j);
                const auto b = a + segments + 1;
                for (const std::uint32_t v : {a, a + 1, b, a + 1, b + 1, b}) {
                    l.push_back(v);
                }
            }
        }
    }
};

struct View {
    std::string name;
    std::array<double, 3> eye, target;
};

void write_scene(const std::filesystem::path& dir, const Scene& s, const std::vector<View>& views) {
    std::filesystem::create_directories(dir / "assets");
    std::filesystem::create_directories(dir / "scripts");
    std::vector<char> bin;
    const auto put = [&](const void* p, std::size_t n) {
        const auto* ch = static_cast<const char*>(p);
        bin.insert(bin.end(), ch, ch + n);
    };
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& p : s.pos) {
        for (std::size_t a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    put(s.pos.data(), s.pos.size() * 12);
    put(s.nrm.data(), s.nrm.size() * 12);
    std::string views_json = std::format(
        "{{\"buffer\":0,\"byteOffset\":0,\"byteLength\":{},\"target\":34962}},"
        "{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{},\"target\":34962}}",
        s.pos.size() * 12, s.pos.size() * 12, s.nrm.size() * 12);
    std::string accessors = std::format(
        "{{\"bufferView\":0,\"componentType\":5126,\"count\":{},\"type\":"
        "\"VEC3\",\"min\":[{},{},{}],\"max\":[{},{},{}]"
        "}},{{\"bufferView\":1,\"componentType\":5126,\"count\":{},\"type\":"
        "\"VEC3\"}}",
        s.pos.size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], s.nrm.size());
    std::string prims, materials;
    int accessor = 2, view = 2;
    for (std::size_t m = 0; m < s.colors.size(); ++m) {
        materials += std::format(
            "{}{{\"pbrMetallicRoughness\":{{\"baseColorFactor\":[{},{},{},1],"
            "\"metallicFactor\":0,"
            "\"roughnessFactor\":0.85}}}}",
            m == 0 ? "" : ",", s.colors[m][0], s.colors[m][1], s.colors[m][2]);
        if (m >= s.idx.size() || s.idx[m].empty()) {
            continue;
        }
        const std::size_t off = bin.size();
        put(s.idx[m].data(), s.idx[m].size() * 4);
        views_json += std::format(
            ",{{\"buffer\":0,\"byteOffset\":{},"
            "\"byteLength\":{},\"target\":34963}}",
            off, s.idx[m].size() * 4);
        accessors += std::format(
            ",{{\"bufferView\":{},\"componentType\":5125,"
            "\"count\":{},\"type\":\"SCALAR\"}}",
            view++, s.idx[m].size());
        prims += std::format(
            "{}{{\"attributes\":{{\"POSITION\":0,\"NORMAL\":1}},"
            "\"indices\":{},\"material\":{}}}",
            prims.empty() ? "" : ",", accessor++, m);
    }
    std::ofstream(dir / "assets/terrain.bin", std::ios::binary)
        .write(bin.data(), static_cast<std::streamsize>(bin.size()));
    std::ofstream(dir / "assets/terrain.gltf") << std::format(
        "{{\"asset\":{{\"version\":\"2.0\",\"generator\":\"veach-"
        "bench-seams\"}},\"scene\":0,"
        "\"scenes\":[{{\"nodes\":[0]}}],\"nodes\":[{{\"mesh\":0}}]"
        ",\"meshes\":[{{\"primitives\":[{}]}}],"
        "\"materials\":[{}],\"accessors\":[{}],\"bufferViews\":[{}"
        "],\"buffers\":[{{\"uri\":\"terrain.bin\","
        "\"byteLength\":{}}}]}}\n",
        prims, materials, accessors, views_json, bin.size());
    std::ofstream(dir / "veach.project.json") << std::format(
        "{{\n  \"name\": \"seams\",\n  \"script\": "
        "\"scripts/{}.luau\",\n  \"window\": {{ \"title\": "
        "\"seams\", \"width\": 1280, \"height\": 720 }}\n}}\n",
        views.front().name);
    for (const View& cam : views) {
        std::ofstream(dir / std::format("scripts/{}.luau", cam.name)) << std::format(
            "--!strict\n-- Generated by veach-bench-seams: camera '{}'. +Y "
            "is radial up at the site, -Z its north.\n"
            "local veach = require(\"@veach\")\nlocal c = veach.components\n"
            "veach.spawn(\"terrain\"):set(c.SceneInstance, {{ path = "
            "\"assets/terrain.gltf\" }})\n"
            "local eye = vector.create({}, {}, {})\nlocal target = "
            "vector.create({}, {}, {})\n"
            "local camera = veach.spawn(\"camera\")\n"
            "camera:set(c.Transform, {{ translation = eye, rotation = "
            "veach.quat.look_rotation(target - eye), "
            "scale = vector.one }})\n"
            "camera:set(c.Camera, {{ fov_y = math.rad(60), near_plane = "
            "0.1, active = true, ev100 = 13 }})\n"
            "local sun = veach.spawn(\"sun\")\n"
            "sun:set(c.Transform, {{ translation = vector.zero, rotation = "
            "veach.quat.look_rotation(vector.create(-0.3, -1, -0.45)), "
            "scale = vector.one }})\n"
            "sun:set(c.DirectionalLight, {{ color = vector.create(1, 0.97, "
            "0.92), illuminance = 40000 }})\n"
            "veach.system(\"Size\", \"Startup\", veach.query(c.Window), "
            "function(e, w)\n"
            "\te:set(c.Window, {{ title = w.title, width = 1280, height = "
            "720, mode = w.mode, vsync = w.vsync }})\n"
            "end)\n",
            cam.name, cam.eye[0], cam.eye[1], cam.eye[2], cam.target[0], cam.target[1], cam.target[2]);
    }
    std::printf("wrote %s (%zu vertices)\n", dir.string().c_str(), s.pos.size());
}

/// Eight colors for the eight LOD0 chunks around a corner: bit a of the index is the key's parity on axis a.
std::size_t parity_color(const voxel::ChunkKey& k) {
    return static_cast<std::size_t>((k.x & 1) | ((k.y & 1) << 1) | ((k.z & 1) << 2));
}
const std::vector<std::array<float, 3>> parity_palette{
    {0.90f, 0.30f, 0.25f}, {0.20f, 0.50f, 0.90f}, {0.30f, 0.75f, 0.25f}, {0.95f, 0.80f, 0.15f},
    {0.60f, 0.30f, 0.85f}, {0.95f, 0.50f, 0.10f}, {0.15f, 0.75f, 0.75f}, {0.50f, 0.50f, 0.50f},
};

Scene scene_at(const Site& site) {
    Scene s;
    for (std::size_t a = 0; a < 3; ++a) {
        s.origin[a] = static_cast<double>(site.center_mm[a]) / 1000.0;
    }
    s.east = site.east;
    s.up = site.up;
    s.north = site.north;
    return s;
}

void shots(const std::filesystem::path& out) {
    const auto site_named = [](std::string_view name) {
        for (const SiteSpec& s : sweep_sites()) {
            if (s.name == name) {
                return make_site(s);
            }
        }
        return make_site(control_site());
    };
    // 1. The ball in the dug pit at the north pole, chunks colored by key parity: the four chunk columns that meet on
    // the spin axis (x = 0 and z = 0) and the character standing next to the pit.
    {
        const Site np = site_named("north_pole");
        sp12::ScenarioConfig c;
        c.site = np;
        c.keep_meshes = true;
        c.workers = 0;
        const sp12::ScenarioResult r = sp12::run_scenario(c);
        Scene s = scene_at(np);
        s.colors = parity_palette;
        s.colors.push_back({0.95f, 0.95f, 0.95f});  // 8: the ball
        s.colors.push_back({0.10f, 0.10f, 0.12f});  // 9: the character
        for (const voxel::ChunkMesh& m : r.meshes) {
            s.add_mesh(m, parity_color(m.key));
        }
        const auto o = voxel::chunk_origin(sp12::chunk_of_mm(np.center_mm));
        std::array<double, 3> ball{};
        for (std::size_t a = 0; a < 3; ++a) {
            ball[a] = static_cast<double>(o[a]) * voxel::lod0_cell_meters + r.ball[a];
        }
        // The sphere's center is 0.4 m above its bottom: stand it on ball − 0.4 up.
        s.add_capsule({ball[0] - 0.4 * np.up[0], ball[1] - 0.4 * np.up[1], ball[2] - 0.4 * np.up[2]}, np.up, 0.4, 0.0,
                      8);
        const std::array<double, 3> feet = surface_point({s.origin[0] + 4.5 * np.east[0] + 1.0 * np.north[0],
                                                          s.origin[1] + 4.5 * np.east[1] + 1.0 * np.north[1],
                                                          s.origin[2] + 4.5 * np.east[2] + 1.0 * np.north[2]});
        s.add_capsule(feet, normalize3(feet), 0.3, 0.6, 9);
        write_scene(
            out / "pole", s,
            {{"pole_pit", {2.5, 7.5, 6.5}, {0.0, -3.0, 0.0}}, {"pole_overview", {9.0, 4.5, 12.0}, {1.5, -1.5, 0.0}}});
    }
    // 2. The multi-level octree across the x = 0 plane: colored by level, darker on the negative side.
    {
        const Site px = site_named("plane_x");
        LodConfig c;
        c.site = px;
        c.root_level = 3;
        c.keep_meshes = true;
        const LodResult r = run_lod(c);
        Scene s = scene_at(px);
        const std::array<std::array<float, 3>, 4> level{
            {{0.85f, 0.30f, 0.25f}, {0.90f, 0.60f, 0.20f}, {0.35f, 0.70f, 0.35f}, {0.30f, 0.50f, 0.85f}}};
        for (const auto& l : level) {
            s.colors.push_back(l);
        }
        for (const auto& l : level) {
            s.colors.push_back({0.45f * l[0], 0.45f * l[1], 0.45f * l[2]});
        }
        for (const sp08::MeshedChunk& m : r.meshes) {
            const std::size_t lv = std::min<std::size_t>(m.mesh.key.level, 3);
            s.add_mesh(m.mesh, lv + (m.mesh.key.x < 0 ? 4 : 0));
        }
        write_scene(out / "lod", s,
                    {{"lod_plane_x", {0.0, 150.0, 150.0}, {0.0, -10.0, 0.0}},
                     {"lod_plane_x_far", {0.0, 260.0, 260.0}, {0.0, -20.0, 0.0}}});
    }
    // 3. A character on an 8-chunk corner near the plane_x site, chunks colored by key parity.
    {
        const Site px = site_named("plane_x");
        const std::array<double, 3> corner = find_chunk_corner(px, 120.0);
        std::array<std::int64_t, 3> corner_mm{};
        for (std::size_t a = 0; a < 3; ++a) {
            corner_mm[a] = static_cast<std::int64_t>(std::floor(corner[a] * 1000.0 + 0.5));
        }
        const sp12::PlanetField field;
        sp12::Terrain terrain(field, sp12::chunk_of_mm(corner_mm), 1);
        JobSystem jobs(0);
        terrain.load(jobs);
        Site at = px;
        at.center_mm = corner_mm;
        Scene s = scene_at(at);
        s.colors = parity_palette;
        s.colors.push_back({0.10f, 0.10f, 0.12f});  // 8: the character
        for (const sp12::Chunk& ch : terrain.chunks()) {
            if (!ch.mesh.indices.empty()) {
                s.add_mesh(ch.mesh, parity_color(ch.mesh.key));
            }
        }
        const std::array<double, 3> feet = surface_point(corner);
        s.add_capsule(feet, normalize3(feet), 0.3, 0.6, 8);
        std::printf(
            "8-chunk corner at (%.1f, %.1f, %.1f) m, %.3f m from the surface\n", corner[0], corner[1], corner[2],
            std::sqrt((feet[0] - corner[0]) * (feet[0] - corner[0]) + (feet[1] - corner[1]) * (feet[1] - corner[1]) +
                      (feet[2] - corner[2]) * (feet[2] - corner[2])));
        write_scene(out / "corner", s, {{"corner", {4.0, 6.0, 7.0}, {0.0, 0.5, 0.0}}});
    }
}
void print_gradient(const char* what, const cubemap::Gradient& g) {
    std::printf(
        "  %-30s within: %6zu pairs, p99 %10.4g max %10.4g, changed "
        "%5.1f %% | across: %4zu pairs, p99 %10.4g "
        "max %10.4g, changed %5.1f %%\n",
        what, g.within, g.within_p99, g.within_max, 100.0 * g.within_changed, g.across, g.across_p99, g.across_max,
        100.0 * g.across_changed);
}

/// The cube-mapped procgen inputs across face edges: SP-07's climate map and SP-06's field graph.
void cube() {
    constexpr int n = static_cast<int>(sp07::face_size);
    const std::vector<cubemap::Pair> pairs = cubemap::neighbor_pairs(n);
    const cubemap::Gradient spacing =
        cubemap::gradient(std::span<const double>(std::vector<double>(pairs.size())), pairs);
    std::printf(
        "SP-07 cube map %d² × 6: neighbor spacing within faces %.5f rad, "
        "across edges %.5f rad\n",
        n, spacing.within_angle, spacing.across_angle);
    const sp07::Body body = sp07::pick_body();
    const sp07::Inputs in = sp07::make_inputs();
    sp07::Outputs out;
    procgen::Sandbox sandbox;
    auto hook = sandbox.load(sp07::read_hook(), "sp07_climate.luau");
    if (!hook || !sp07::run(sandbox, *hook, body, in, out)) {
        std::printf("the SP-07 hook failed\n");
        return;
    }
    print_gradient("latitude (rad/rad)", cubemap::gradient(std::span<const double>(in.latitude), pairs));
    print_gradient("altitude (m/rad)", cubemap::gradient(std::span<const double>(in.altitude), pairs));
    print_gradient("temperature (K/rad)", cubemap::gradient(std::span<const double>(out.temperature), pairs));
    print_gradient("precipitation (mm/yr/rad)", cubemap::gradient(std::span<const double>(out.precipitation), pairs));
    print_gradient("biome (id)", cubemap::gradient(std::span<const std::uint8_t>(out.biome), pairs));

    auto json = sp06::terrain_graph_json();
    if (!json) {
        return;
    }
    auto program = sp06::compile(*json, {.seed = 42, .radius_planet_m = 6.0e6});
    if (!program) {
        return;
    }
    for (const int res : {64, 256}) {
        const std::vector<cubemap::Pair> p = cubemap::neighbor_pairs(res);
        const std::vector<float> map = cubemap::field_graph_map(*program, res);
        char label[64];
        std::snprintf(label, sizeof label, "SP-06 terrain graph at %d²", res);
        print_gradient(label, cubemap::gradient(std::span<const float>(map), p));
    }
    // An exact edge point from both faces' parameterizations: c + u eu + v ev with u, v = ±1 and 0/±1 axes is the same
    // float vector, so the same normalized position and value.
    std::size_t edge_points = 0, edge_mismatches = 0;
    for (int f = 0; f < 6; ++f) {
        for (int g = f + 1; g < 6; ++g) {
            for (int k = 0; k <= 64; ++k) {
                const double t = 2.0 * k / 64.0 - 1.0;
                // Points on face f's four edges that also lie on face g.
                for (const auto& [u, v] :
                     {std::pair{1.0, t}, std::pair{-1.0, t}, std::pair{t, 1.0}, std::pair{t, -1.0}}) {
                    const auto& A = cubemap::faces[static_cast<std::size_t>(f)];
                    std::array<float, 3> pa{};
                    for (std::size_t a = 0; a < 3; ++a) {
                        pa[a] = static_cast<float>(A.c[a]) + static_cast<float>(u) * static_cast<float>(A.eu[a]) +
                                static_cast<float>(v) * static_cast<float>(A.ev[a]);
                    }
                    const auto& B = cubemap::faces[static_cast<std::size_t>(g)];
                    const float dc = pa[0] * static_cast<float>(B.c[0]) + pa[1] * static_cast<float>(B.c[1]) +
                                     pa[2] * static_cast<float>(B.c[2]);
                    if (dc != 1.0f) {
                        continue;  // not on face g's plane
                    }
                    const float ub = pa[0] * static_cast<float>(B.eu[0]) + pa[1] * static_cast<float>(B.eu[1]) +
                                     pa[2] * static_cast<float>(B.eu[2]);
                    const float vb = pa[0] * static_cast<float>(B.ev[0]) + pa[1] * static_cast<float>(B.ev[1]) +
                                     pa[2] * static_cast<float>(B.ev[2]);
                    std::array<float, 3> pb{};
                    for (std::size_t a = 0; a < 3; ++a) {
                        pb[a] = static_cast<float>(B.c[a]) + ub * static_cast<float>(B.eu[a]) +
                                vb * static_cast<float>(B.ev[a]);
                    }
                    const auto value = [&](const std::array<float, 3>& q) {
                        const float l = std::sqrt(std::fma(q[0], q[0], std::fma(q[1], q[1], q[2] * q[2])));
                        return sp06::evaluate_point(*program, q[0] / l, q[1] / l, q[2] / l);
                    };
                    ++edge_points;
                    edge_mismatches += (pa != pb || value(pa) != value(pb)) ? 1 : 0;
                }
            }
        }
    }
    std::printf("  exact edge points evaluated from both faces: %zu, differing %zu\n", edge_points, edge_mismatches);
}
/// SP-13's landing (autopilot, walkers on) with the pad moved toward the north pole.
void pole() {
    std::printf("%-6s %-9s %-6s %7s %9s %9s %10s %8s %11s %9s %9s\n", "scale", "latitude", "done", "ticks", "hover min",
                "hover max", "touchdown", "offset", "walker ok", "h min mm", "h max mm");
    for (const double scale : {0.01, 1.0}) {
        for (const double latitude : {30.0, 89.0, 89.99, 89.9999, 90.0}) {
            sp13::Sim sim(sp13::Config{.scale = scale, .latitude = latitude});
            sp13::Autopilot pilot(sim.planet(), false);
            sp13::Controls c;
            bool supported = true, finite = true;
            double hmin = 1e300, hmax = -1e300;
            for (int i = 0; i < 60 * 600 && pilot.phase() != sp13::Autopilot::Phase::done; ++i) {
                sim.step(c);
                c = pilot.next(sim.craft());
                const math::dvec3 p = sim.craft_position_body();
                finite = finite && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
                if (!finite) {
                    break;
                }
                if (sim.tick() > 300) {  // the walker starts 2 cm up
                    supported = supported && sim.walker_supported();
                    const double h = math::length(sim.position_body("walker")) - sim.planet().radius;
                    hmin = std::min(hmin, h);
                    hmax = std::max(hmax, h);
                }
            }
            const bool done = pilot.phase() == sp13::Autopilot::Phase::done;
            std::printf(
                "%-6.2f %-9.4f %-6s %7lld %9.2f %9.2f %10.3f %8.3f %11s "
                "%9.2f %9.2f%s\n",
                scale, latitude, done ? "yes" : "no", static_cast<long long>(sim.tick()), pilot.hover_min(),
                pilot.hover_max(), pilot.touchdown_speed(), pilot.landing_offset(), supported ? "always" : "lost",
                hmin * 1000.0, hmax * 1000.0, finite ? "" : "  (craft position not finite)");
        }
    }
}
/// Per-step |Δv| of every walk in the five collider variants, as CSV files in `dir` (walk<i>_<variant>.csv).
void trace(const std::filesystem::path& dir, int legs) {
    std::filesystem::create_directories(dir);
    int index = 0;
    for (WalkConfig w : sweep_walks()) {
        w.legs = legs;
        w.trace = true;
        for (const int variant : {0, 1, 2, 3, 4}) {
            w.merged = variant == 1;
            w.edge_removal = variant == 2;
            w.ring = variant == 3 ? Ring::duplicate : (variant == 4 ? Ring::adjacency : Ring::none);
            const WalkResult r = run_walk(w);
            std::ofstream f(dir / std::format("walk{}_{}.csv", index, variant_names[variant]));
            f << "x,y,z,dv,dv_up,boundary\n";
            for (const auto& s : r.trace) {
                f << std::format("{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{}\n", s.feet[0], s.feet[1], s.feet[2], s.dv,
                                 s.dv_up, s.boundary ? 1 : 0);
            }
        }
        std::printf("walk%d: %s\n", index, std::string(w.name).c_str());
        ++index;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "all";
    std::string out;
    int legs = 4;
    int root_level = 3;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--root-level") == 0 && i + 1 < argc) {
            root_level = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--legs") == 0 && i + 1 < argc) {
            legs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else {
            mode = argv[i];
        }
    }
    if (mode == "shots") {
        shots(out.empty() ? "seams-shots" : out);
        return 0;
    }
    if (mode == "cubemap") {
        cube();
        return 0;
    }
    if (mode == "pole") {
        pole();
        return 0;
    }
    if (mode == "trace") {
        trace(out.empty() ? "seams-trace" : out, legs);
        return 0;
    }
    if (mode == "lod" || mode == "all") {
        lod(root_level);
    }
    if (mode == "dig" || mode == "all") {
        dig();
    }
    if (mode == "walk" || mode == "all") {
        walk(legs);
    }
    return 0;
}
