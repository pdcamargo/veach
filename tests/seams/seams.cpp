// veach: canonical
// Seams sweep, throwaway: sites, the multi-level weld and the field's continuity (seams.hpp).

#include "seams/seams.hpp"

#include <veach/core/jobs.hpp>
#include <veach/voxel/octree.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <unordered_map>

namespace veach::seams {

using namespace voxel;

namespace {

constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

double dot(const Vec& a, const Vec& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec cross(const Vec& a, const Vec& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec normalize(const Vec& v) {
    const double l = std::sqrt(dot(v, v));
    return {v[0] / l, v[1] / l, v[2] / l};
}
Vec add3(const Vec& a, const Vec& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

const sp12::PlanetField& field() {
    static const sp12::PlanetField f;
    return f;
}

/// The radius where SP-12's field crosses 0 along unit direction `n`: bisection over the relief band.
double surface_radius(const Vec& n) {
    double lo = sp12::PlanetField::radius - sp12::PlanetField::relief - 1.0;
    double hi = sp12::PlanetField::radius + sp12::PlanetField::relief + 1.0;
    for (int i = 0; i < 64; ++i) {
        const double mid = 0.5 * (lo + hi);
        (field().distance(n[0] * mid, n[1] * mid, n[2] * mid) < 0.0 ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

}  // namespace

const std::vector<SiteSpec>& sweep_sites() {
    static const std::vector<SiteSpec> sites{
        {"north_pole", {0.0, 1.0, 0.0}, "+Y, the spin axis: on the x = 0 and z = 0 planes"},
        {"south_pole", {0.0, -1.0, 0.0}, "-Y: on the x = 0 and z = 0 planes"},
        {"plane_x", {0.0, -0.6, -0.8}, "on x = 0, y and z negative"},
        {"plane_y", {-0.6, 0.0, -0.8}, "on y = 0 (the equator), x and z negative"},
        {"plane_z", {-0.6, -0.8, 0.0}, "on z = 0, x and y negative"},
        {"corner_neg", {-1.0, -1.0, -1.0}, "(-1, -1, -1)/sqrt(3), a cube-corner direction"},
        {"corner_pos", {1.0, 1.0, 1.0}, "(1, 1, 1)/sqrt(3), a cube-corner direction"},
        {"lon180", {-1.0, 0.0, 0.0}, "the equator at longitude 180 degrees: on the y = 0 and z = 0 planes"},
    };
    return sites;
}

SiteSpec control_site() {
    return {"sp12", {0.6, 0.8, 0.0}, "SP-12's site, the control"};
}

Site make_site(const SiteSpec& spec) {
    const Vec n = normalize(spec.direction);
    const double r = surface_radius(n);
    Site s;
    for (std::size_t a = 0; a < 3; ++a) {
        s.center_mm[a] = static_cast<std::int64_t>(std::nearbyint(n[a] * r * 1000.0));
        s.up[a] = n[a];
    }
    const Vec e = cross({0.0, 1.0, 0.0}, n);
    s.east = dot(e, e) > 1e-24 ? normalize(e) : Vec{1.0, 0.0, 0.0};  // at a pole, "east" is a convention
    s.north = cross(s.up, s.east);
    return s;
}

unsigned octant_planes(const Site& site) {
    unsigned m = 0;
    for (unsigned a = 0; a < 3; ++a) {
        if (site.center_mm[a] == 0) {
            m |= 1u << a;
        }
    }
    return m;
}

Vec surface_point(const Vec& p) {
    const Vec n = normalize(p);
    const double r = surface_radius(n);
    return {n[0] * r, n[1] * r, n[2] * r};
}

// --- The multi-level weld -----------------------------------------------------------------------------------

namespace {

struct PointHash {
    std::size_t operator()(const std::array<std::int64_t, 3>& p) const {
        std::uint64_t h = 0x9e3779b97f4a7c15ULL;
        for (const std::int64_t v : p) {
            h ^= static_cast<std::uint64_t>(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return static_cast<std::size_t>(h);
    }
};

}  // namespace

LodResult run_lod(const LodConfig& config) {
    LodResult r;
    const sp12::PlanetField& f = field();
    std::array<std::int64_t, 3> site{};
    LodSettings settings;
    settings.lambda = config.lambda;
    settings.root_level = config.root_level;
    const std::int64_t root_edge = chunk_units(config.root_level);
    for (std::size_t a = 0; a < 3; ++a) {
        site[a] = floor_div(config.site.center_mm[a], lod0_cell_mm);
        // The focus: the site, 2 m up (a character's eyes).
        settings.focus[a] = static_cast<double>(config.site.center_mm[a]) / 500.0 + 4.0 * config.site.up[a];
        const std::int64_t k = floor_div(site[a], root_edge);
        settings.root_min[a] = static_cast<std::int32_t>(k - config.roots_half);
        settings.root_max[a] = static_cast<std::int32_t>(k + config.roots_half);
        r.region_lo[a] = settings.root_min[a] * root_edge;
        r.region_hi[a] = (settings.root_max[a] + 1) * root_edge;
    }
    const std::vector<ChunkLeaf> leaves = select_chunks(settings);
    r.leaves = leaves.size();

    JobSystem jobs(config.workers);
    std::vector<std::unique_ptr<IsoMesher>> meshers;
    std::vector<ChunkSamples> scratch(jobs.thread_count());
    for (std::uint32_t i = 0; i < jobs.thread_count(); ++i) {
        meshers.push_back(make_transvoxel_mesher({}));
    }
    // Samples within one cell of an octant plane the site lies on, per chunk, for the cross-plane check.
    const unsigned planes = octant_planes(config.site);
    struct Held {
        std::array<std::int64_t, 3> p;
        Sample s;
    };
    std::vector<std::vector<Held>> held(leaves.size());
    std::vector<sp08::MeshedChunk> meshed(leaves.size());
    std::vector<std::uint8_t> sampled(leaves.size(), 0);
    jobs.parallel_for("seams lod", leaves.size(), 1, [&](std::size_t begin, std::size_t end) {
        const std::uint32_t t = jobs.thread_index();
        for (std::size_t i = begin; i < end; ++i) {
            meshed[i].leaf = leaves[i];
            meshed[i].mesh.key = leaves[i].key;
            if (!f.may_have_surface(leaves[i].key)) {
                continue;
            }
            sampled[i] = 1;
            ChunkSamples& s = scratch[t];
            sample_chunk(f, leaves[i].key, meshers[t]->face_samples_needed(leaves[i].neighbors), s);
            meshers[t]->mesh(s, leaves[i].neighbors, meshed[i].mesh);
            if (planes == 0) {
                continue;
            }
            const auto o = chunk_origin(s.key);
            const std::int64_t c = cell_units(s.key.level);
            const auto near_plane = [&](const std::array<std::int64_t, 3>& p, std::int64_t reach) {
                for (std::size_t a = 0; a < 3; ++a) {
                    if ((planes >> a & 1u) != 0 && p[a] >= -reach && p[a] <= reach) {
                        return true;
                    }
                }
                return false;
            };
            for (int z = -1; z <= chunk_cells + 1; ++z) {
                for (int y = -1; y <= chunk_cells + 1; ++y) {
                    for (int x = -1; x <= chunk_cells + 1; ++x) {
                        const std::array<std::int64_t, 3> p{o[0] + x * c, o[1] + y * c, o[2] + z * c};
                        if (near_plane(p, c)) {
                            held[i].push_back({p, s.at(x, y, z)});
                        }
                    }
                }
            }
            for (int face = 0; face < 6; ++face) {
                if (s.faces[static_cast<std::size_t>(face)].empty()) {
                    continue;
                }
                const int a = face / 2, b = (a + 1) % 3, cc = (a + 2) % 3;
                const std::int64_t h = c / 2;
                const std::int64_t plane = o[static_cast<std::size_t>(a)] + ((face & 1) ? chunk_units(s.key.level) : 0);
                for (int layer = -1; layer <= 1; ++layer) {
                    for (int v = -1; v <= 2 * chunk_cells + 1; ++v) {
                        for (int u = -1; u <= 2 * chunk_cells + 1; ++u) {
                            std::array<std::int64_t, 3> p{};
                            p[static_cast<std::size_t>(a)] = plane + layer * h;
                            p[static_cast<std::size_t>(b)] = o[static_cast<std::size_t>(b)] + u * h;
                            p[static_cast<std::size_t>(cc)] = o[static_cast<std::size_t>(cc)] + v * h;
                            if (near_plane(p, h)) {
                                held[i].push_back({p, s.face_at(face, u, v, layer)});
                            }
                        }
                    }
                }
            }
        }
    });

    std::vector<std::uint64_t> hashes;
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        hashes.push_back(voxel::mesh_hash(meshed[i].mesh));
        r.sampled += sampled[i];
        if (!meshed[i].mesh.indices.empty()) {
            ++r.meshed;
            ++r.meshed_per_level[std::min<std::size_t>(leaves[i].key.level, r.meshed_per_level.size() - 1)];
            r.transition_triangles += meshed[i].mesh.transition_triangles;
        }
    }
    r.mesh_hash = combine_hashes(hashes);
    r.seams = sp08::check_seams(meshed, r.region_lo, r.region_hi);

    // Every lattice point near the plane, as held by each chunk (a chunk's own grid repeats no point).
    struct Seen {
        Sample s;
        std::uint32_t chunks = 0;
        bool mismatch = false;
    };
    std::unordered_map<std::array<std::int64_t, 3>, Seen, PointHash> seen;
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        std::map<std::array<std::int64_t, 3>, Sample> own;  // a face and the grid may hold the same point
        for (const Held& h : held[i]) {
            own.emplace(h.p, h.s);
        }
        for (const auto& [p, s] : own) {
            auto [it, inserted] = seen.try_emplace(p, Seen{s, 0, false});
            ++it->second.chunks;
            if (it->second.s.distance != s.distance || it->second.s.material != s.material) {
                it->second.mismatch = true;
            }
        }
    }
    for (const auto& [p, s] : seen) {
        if (s.chunks >= 2) {
            ++r.shared_points;
            r.shared_mismatches += s.mismatch ? 1 : 0;
        }
    }
    if (config.keep_meshes) {
        for (auto& m : meshed) {
            if (!m.mesh.indices.empty()) {
                r.meshes.push_back(std::move(m));
            }
        }
    }
    return r;
}

// --- Continuity across an octant plane ------------------------------------------------------------------------

Continuity check_continuity(const Site& site, int axis) {
    const sp12::PlanetField& f = field();
    Continuity c;
    // |∇d| ≤ 1 + |∇noise|: value noise with a smoothstep fade has |∂/∂x| ≤ 1.5
    // per wavelength, ×2 for [-1, 1), over three axes and the three octaves
    // (6/48, 1.5/12, 0.35/3 per meter).
    c.lipschitz_bound = 0.5 * (1.0 + 3.0 * std::sqrt(3.0) * (6.0 / 48.0 + 1.5 / 12.0 + 0.35 / 3.0));
    const auto ax = static_cast<std::size_t>(axis);
    Vec e{};
    e[ax] = 1.0;
    // The curve where the plane meets the surface: through the site along t = e ×
    // up (in the plane, tangent).
    const Vec t = normalize(cross(e, site.up));
    const Vec center{static_cast<double>(site.center_mm[0]) / 1000.0, static_cast<double>(site.center_mm[1]) / 1000.0,
                     static_cast<double>(site.center_mm[2]) / 1000.0};
    for (int i = -20; i <= 20; ++i) {
        Vec q{center[0] + 2.0 * i * t[0], center[1] + 2.0 * i * t[1], center[2] + 2.0 * i * t[2]};
        q[ax] = 0.0;
        const Vec s = surface_point(q);
        std::array<std::int64_t, 3> p{};
        for (std::size_t a = 0; a < 3; ++a) {
            p[a] = static_cast<std::int64_t>(std::floor(s[a] / lod0_cell_meters + 0.5));
        }
        p[ax] = 0;
        ++c.lines;
        // Lattice points k = -8..8 along the axis: steps across the plane (k = -1
        // to 0, 0 to 1) against steps within one side (|k| >= 2).
        double prev = 0.0;
        for (int k = -8; k <= 8; ++k) {
            std::array<std::int64_t, 3> l = p;
            l[ax] = k;
            const double d = static_cast<double>(f.sample(l[0], l[1], l[2]).distance) * distance_quantum;
            if (k > -8) {
                const double step = std::abs(d - prev);
                if (k == 0 || k == 1) {
                    c.max_step_across = std::max(c.max_step_across, step);
                } else if (k >= 3 || k <= -2) {
                    c.max_step_within = std::max(c.max_step_within, step);
                }
            }
            prev = d;
        }
        // The unquantized field 1 µm either side of the plane.
        Vec plus{static_cast<double>(p[0]) * 0.5, static_cast<double>(p[1]) * 0.5, static_cast<double>(p[2]) * 0.5};
        Vec minus = plus;
        plus[ax] = 1e-6;
        minus[ax] = -1e-6;
        c.max_jump = std::max(
            c.max_jump, std::abs(f.distance(plus[0], plus[1], plus[2]) - f.distance(minus[0], minus[1], minus[2])));
    }
    return c;
}
Vec find_chunk_corner(const Site& site, double radius) {
    const sp12::PlanetField& f = field();
    const double edge = static_cast<double>(chunk_units(0)) * lod0_cell_meters;  // 16 m
    Vec best{};
    double best_d = 1e300;
    const Vec center{static_cast<double>(site.center_mm[0]) / 1000.0, static_cast<double>(site.center_mm[1]) / 1000.0,
                     static_cast<double>(site.center_mm[2]) / 1000.0};
    const int n = static_cast<int>(radius / edge) + 1;
    std::array<std::int64_t, 3> k0{};
    for (std::size_t a = 0; a < 3; ++a) {
        k0[a] = static_cast<std::int64_t>(std::floor(center[a] / edge + 0.5));
    }
    for (int i = -n; i <= n; ++i) {
        for (int j = -n; j <= n; ++j) {
            for (int k = -n; k <= n; ++k) {
                const Vec p{static_cast<double>(k0[0] + i) * edge, static_cast<double>(k0[1] + j) * edge,
                            static_cast<double>(k0[2] + k) * edge};
                const Vec d{p[0] - center[0], p[1] - center[1], p[2] - center[2]};
                const double along = dot(d, site.up);
                if (dot(d, d) - along * along > radius * radius) {
                    continue;
                }
                const double dist = std::abs(f.distance(p[0], p[1], p[2]));
                if (dist < best_d) {
                    best_d = dist;
                    best = p;
                }
            }
        }
    }
    return best;
}

}  // namespace veach::seams

namespace veach::seams {

std::vector<WalkConfig> sweep_walks() {
    const auto site = [](std::string_view name) {
        for (const SiteSpec& s : sweep_sites()) {
            if (s.name == name) {
                return make_site(s);
            }
        }
        return make_site(control_site());
    };
    const auto center = [](const Site& s) {
        return Vec{static_cast<double>(s.center_mm[0]) / 1000.0, static_cast<double>(s.center_mm[1]) / 1000.0,
                   static_cast<double>(s.center_mm[2]) / 1000.0};
    };
    const auto tangent = [](const Site& s, const Vec& v) {
        const double d = dot(v, s.up);
        return normalize({v[0] - d * s.up[0], v[1] - d * s.up[1], v[2] - d * s.up[2]});
    };
    std::vector<WalkConfig> walks;
    const auto walk = [&](std::string_view name, const Site& s, const Vec& through, const Vec& direction) {
        WalkConfig w;
        w.name = name;
        w.site = s;
        w.through = through;
        w.direction = tangent(s, direction);
        walks.push_back(w);
    };
    const Site px = site("plane_x"), py = site("plane_y"), pz = site("plane_z");
    walk("across x = 0", px, center(px), {1.0, 0.0, 0.0});
    walk("across y = 0", py, center(py), {0.0, 1.0, 0.0});
    walk("across z = 0", pz, center(pz), {0.0, 0.0, 1.0});
    walk("through an 8-chunk corner", px, find_chunk_corner(px, 120.0), add3(px.east, px.north));
    const Site np = site("north_pole"), sp = site("south_pole"), lon = site("lon180");
    walk("over the north pole, diagonally", np, center(np), {1.0, 0.0, 1.0});
    walk("over the north pole, along z = 0", np, center(np), {1.0, 0.0, 0.0});
    walk("over the south pole, diagonally", sp, center(sp), {1.0, 0.0, 1.0});
    walk("across longitude 180 at the equator", lon, center(lon), {0.0, 1.0, 1.0});
    const Site cn = site("corner_neg");
    walk("at (-1, -1, -1)/sqrt(3)", cn, center(cn), cn.east);
    return walks;
}

}  // namespace veach::seams
