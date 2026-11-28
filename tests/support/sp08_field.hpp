#pragma once

// Spike SP-08 (VCH-084), throwaway: the test body and the seam checks shared by tests/voxel_tests.cpp and
// tests/bench/sp08_mesher_bench.cpp (docs/research/spikes/sp-08-mesher.md). Canonical (ADR 0043): + − × ÷ √ and floor
// only, randomness from veach::hash.

#include <veach/core/hash.hpp>
#include <veach/voxel/octree.hpp>
#include <veach/voxel/voxel.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <thread>
#include <vector>

namespace sp08 {

using namespace veach::voxel;

/// A rocky sphere of 300 m in body-local meters: three octaves of hash value noise on the radius, and a winding tunnel
/// 7 m in radius carved along x at 282 m height, which opens on the surface about 100 m either side of the top.
struct SphereField final : DensityField {
    static constexpr double radius = 300.0;
    static constexpr double relief = 7.9;  ///< bound on |noise|, meters
    static constexpr double tunnel_y = 282.0;
    static constexpr double tunnel_radius = 7.0;
    static constexpr double tunnel_wobble = 4.0;
    std::uint64_t seed = 0x5108'0001;

    static double smooth(double t) { return t * t * (3.0 - 2.0 * t); }

    double lattice(std::uint64_t octave, std::int64_t x, std::int64_t y, std::int64_t z) const {
        return veach::unit_double(veach::hash(seed, octave, static_cast<std::uint64_t>(x),
                                              static_cast<std::uint64_t>(y), static_cast<std::uint64_t>(z)));
    }

    /// Value noise in [-1, 1) with wavelength 1 in the units of p.
    double noise(std::uint64_t octave, double px, double py, double pz) const {
        const double fx = std::floor(px), fy = std::floor(py), fz = std::floor(pz);
        const auto ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy),
                   iz = static_cast<std::int64_t>(fz);
        const double tx = smooth(px - fx), ty = smooth(py - fy), tz = smooth(pz - fz);
        const auto lerp = [](double a, double b, double t) {
            return a + (b - a) * t;
        };
        const double x00 = lerp(lattice(octave, ix, iy, iz), lattice(octave, ix + 1, iy, iz), tx);
        const double x10 = lerp(lattice(octave, ix, iy + 1, iz), lattice(octave, ix + 1, iy + 1, iz), tx);
        const double x01 = lerp(lattice(octave, ix, iy, iz + 1), lattice(octave, ix + 1, iy, iz + 1), tx);
        const double x11 = lerp(lattice(octave, ix, iy + 1, iz + 1), lattice(octave, ix + 1, iy + 1, iz + 1), tx);
        return 2.0 * lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz) - 1.0;
    }

    /// 1D value noise in [-1, 1) along x.
    double noise1(std::uint64_t octave, double px) const {
        const double fx = std::floor(px);
        const auto ix = static_cast<std::int64_t>(fx);
        const double a = veach::unit_double(veach::hash(seed, octave, static_cast<std::uint64_t>(ix)));
        const double b = veach::unit_double(veach::hash(seed, octave, static_cast<std::uint64_t>(ix + 1)));
        return 2.0 * (a + (b - a) * smooth(px - fx)) - 1.0;
    }

    double distance(double px, double py, double pz) const {
        const double r = std::sqrt(px * px + py * py + pz * pz);
        const double n = 6.0 * noise(1, px / 48.0, py / 48.0, pz / 48.0) +
                         1.5 * noise(2, px / 12.0, py / 12.0, pz / 12.0) +
                         0.35 * noise(3, px / 3.0, py / 3.0, pz / 3.0);
        const double surface = r - radius + n;
        const double cy = tunnel_y + tunnel_wobble * noise1(4, px / 30.0);
        const double cz = tunnel_wobble * noise1(5, px / 37.0);
        const double ay = py - cy, az = pz - cz;
        const double tunnel =
            tunnel_radius + 1.2 * noise(6, px / 6.0, py / 6.0, pz / 6.0) - std::sqrt(ay * ay + az * az);
        return std::max(surface, tunnel);
    }

    Sample sample(std::int64_t x, std::int64_t y, std::int64_t z) const override {
        const double px = static_cast<double>(x) * lod0_cell_meters, py = static_cast<double>(y) * lod0_cell_meters,
                     pz = static_cast<double>(z) * lod0_cell_meters;
        const double d = std::floor(distance(px, py, pz) / distance_quantum + 0.5);
        const double clamped = std::clamp(d, -32767.0, 32767.0);
        const double r2 = px * px + py * py + pz * pz;
        const std::uint8_t material = r2 < (radius - 6.0) * (radius - 6.0) ? 2 : 1;  // rock under regolith
        return Sample{static_cast<std::int16_t>(clamped), material};
    }

    /// False when the chunk's box provably holds no surface (outside the relief, or deep and away from the tunnel).
    bool may_have_surface(const ChunkKey& k) const {
        const auto o = chunk_origin(k);
        const double e = static_cast<double>(chunk_units(k.level)) * lod0_cell_meters;
        double near2 = 0.0, far2 = 0.0;
        std::array<double, 3> lo{}, hi{};
        for (std::size_t a = 0; a < 3; ++a) {
            lo[a] = static_cast<double>(o[a]) * lod0_cell_meters;
            hi[a] = lo[a] + e;
            const double n = lo[a] > 0.0 ? lo[a] : (hi[a] < 0.0 ? -hi[a] : 0.0);
            const double f = std::max(std::abs(lo[a]), std::abs(hi[a]));
            near2 += n * n;
            far2 += f * f;
        }
        const double margin = relief + 1.0;
        if (near2 > (radius + margin) * (radius + margin)) {
            return false;  // all empty
        }
        const double band = tunnel_wobble + tunnel_radius + 1.2 + 1.0;
        const bool tunnel = hi[1] >= tunnel_y - band && lo[1] <= tunnel_y + band && hi[2] >= -band && lo[2] <= band;
        return tunnel || far2 >= (radius - margin) * (radius - margin);
    }
};

/// Every leaf meshed: samples, then the mesh, on `threads` workers (one mesher each), results in leaf order.
struct MeshedChunk {
    ChunkLeaf leaf;
    ChunkMesh mesh;
    double sample_ms = 0.0;
    double mesh_ms = 0.0;
};

template <typename MakeMesher, typename Clock>
std::vector<MeshedChunk> mesh_all(const SphereField& field, const std::vector<ChunkLeaf>& leaves, MakeMesher make,
                                  unsigned threads, Clock now) {
    std::vector<MeshedChunk> out(leaves.size());
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        auto mesher = make();
        ChunkSamples samples;
        for (std::size_t i = next++; i < leaves.size(); i = next++) {
            MeshedChunk& m = out[i];
            m.leaf = leaves[i];
            m.mesh.key = leaves[i].key;
            if (!field.may_have_surface(leaves[i].key)) {
                continue;
            }
            const double t0 = now();
            sample_chunk(field, leaves[i].key, mesher->face_samples_needed(leaves[i].neighbors), samples);
            const double t1 = now();
            mesher->mesh(samples, leaves[i].neighbors, m.mesh);
            const double t2 = now();
            m.sample_ms = t1 - t0;
            m.mesh_ms = t2 - t1;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) {
        pool.emplace_back(work);
    }
    work();
    for (auto& t : pool) {
        t.join();
    }
    return out;
}

/// Runs `job(i)` for i in [0, n) on `threads` workers; `make_state()` gives each worker its own state.
template <typename MakeState, typename Job>
void parallel_for(std::size_t n, unsigned threads, MakeState make_state, Job job) {
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        auto state = make_state();
        for (std::size_t i = next++; i < n; i = next++) {
            job(state, i);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) {
        pool.emplace_back(work);
    }
    work();
    for (auto& t : pool) {
        t.join();
    }
}

/// Samples of every leaf that may hold surface, with the full-resolution faces toward finer neighbors.
inline std::vector<ChunkSamples> sample_all(const SphereField& field, const std::vector<ChunkLeaf>& leaves,
                                            unsigned threads) {
    std::vector<ChunkSamples> out(leaves.size());
    parallel_for(
        leaves.size(), threads, [] { return 0; },
        [&](int, std::size_t i) {
            out[i].key = leaves[i].key;
            if (field.may_have_surface(leaves[i].key)) {
                sample_chunk(field, leaves[i].key, leaves[i].neighbors.finer, out[i]);
            }
        });
    return out;
}

/// Meshes pre-sampled chunks (see sample_all) on `threads` workers.
template <typename MakeMesher>
std::vector<MeshedChunk> mesh_samples(const std::vector<ChunkSamples>& samples, const std::vector<ChunkLeaf>& leaves,
                                      MakeMesher make, unsigned threads) {
    std::vector<MeshedChunk> out(leaves.size());
    parallel_for(leaves.size(), threads, make, [&](auto& mesher, std::size_t i) {
        out[i].leaf = leaves[i];
        out[i].mesh.key = leaves[i].key;
        if (!samples[i].grid.empty()) {
            mesher->mesh(samples[i], leaves[i].neighbors, out[i].mesh);
        }
    });
    return out;
}

inline std::uint64_t meshes_hash(const std::vector<MeshedChunk>& chunks) {
    std::vector<std::uint64_t> hashes;
    hashes.reserve(chunks.size());
    for (const auto& c : chunks) {
        hashes.push_back(mesh_hash(c.mesh));
    }
    return combine_hashes(hashes);
}

/// Topological crack test: vertices of all chunks welded by their canonical position; an edge used by one triangle
/// is open (a crack or a hole) unless it lies on the boundary of the meshed region (within one cell of it).
struct SeamReport {
    std::size_t triangles = 0;
    std::size_t open_edges = 0;            ///< used once, not on the region boundary
    std::size_t nonmanifold_edges = 0;     ///< used three or more times
    std::size_t boundary_edges = 0;        ///< open, on the region boundary (expected)
    std::size_t open_at_level_change = 0;  ///< open edges in a chunk that touches another level
    std::size_t wound_out = 0;             ///< triangles whose face normal agrees with the vertex normals
};

inline SeamReport check_seams(const std::vector<MeshedChunk>& chunks, const std::array<std::int64_t, 3>& region_lo,
                              const std::array<std::int64_t, 3>& region_hi) {
    using Pos = std::array<std::int64_t, 3>;
    std::map<Pos, std::uint32_t> weld;
    struct Edge {
        std::uint32_t a, b;
        std::uint32_t chunk;
        auto operator<=>(const Edge&) const = default;
    };
    std::vector<Edge> edges;
    std::vector<Pos> positions;
    SeamReport r;
    for (std::uint32_t ci = 0; ci < chunks.size(); ++ci) {
        const ChunkMesh& m = chunks[ci].mesh;
        std::vector<std::uint32_t> ids(m.fixed.size());
        for (std::size_t v = 0; v < m.fixed.size(); ++v) {
            const auto [it, inserted] = weld.emplace(m.fixed[v], static_cast<std::uint32_t>(positions.size()));
            if (inserted) {
                positions.push_back(m.fixed[v]);
            }
            ids[v] = it->second;
        }
        for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            ++r.triangles;
            std::array<double, 3> e1{}, e2{}, n{};
            for (std::size_t a = 0; a < 3; ++a) {
                e1[a] = static_cast<double>(m.fixed[m.indices[t + 1]][a] - m.fixed[m.indices[t]][a]);
                e2[a] = static_cast<double>(m.fixed[m.indices[t + 2]][a] - m.fixed[m.indices[t]][a]);
                n[a] = static_cast<double>(m.normal[m.indices[t]][a] + m.normal[m.indices[t + 1]][a] +
                                           m.normal[m.indices[t + 2]][a]);
            }
            const double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
                         cz = e1[0] * e2[1] - e1[1] * e2[0];
            if (cx * n[0] + cy * n[1] + cz * n[2] > 0.0) {
                ++r.wound_out;
            }
            for (std::size_t e = 0; e < 3; ++e) {
                const std::uint32_t a = ids[m.indices[t + e]], b = ids[m.indices[t + (e + 1) % 3]];
                if (a != b) {
                    edges.push_back({std::min(a, b), std::max(a, b), ci});
                }
            }
        }
    }
    std::sort(edges.begin(), edges.end());
    for (std::size_t i = 0; i < edges.size();) {
        std::size_t j = i + 1;
        while (j < edges.size() && edges[j].a == edges[i].a && edges[j].b == edges[i].b) {
            ++j;
        }
        const std::size_t uses = j - i;
        if (uses == 1) {
            const Pos& pa = positions[edges[i].a];
            const Pos& pb = positions[edges[i].b];
            // On the region boundary: both ends within one cell of the same boundary plane (marching cubes puts them
            // on it; surface nets half a cell inside).
            const std::int64_t margin = cell_units(chunks[edges[i].chunk].leaf.key.level) << fixed_bits;
            bool on_boundary = false;
            for (std::size_t a = 0; a < 3; ++a) {
                const std::int64_t lo = region_lo[a] << fixed_bits, hi = region_hi[a] << fixed_bits;
                on_boundary = on_boundary || (pa[a] - lo <= margin && pb[a] - lo <= margin) ||
                              (hi - pa[a] <= margin && hi - pb[a] <= margin);
            }
            if (on_boundary) {
                ++r.boundary_edges;
            } else {
                ++r.open_edges;
                const auto& n = chunks[edges[i].chunk].leaf.neighbors;
                if (n.touching) {
                    ++r.open_at_level_change;
                }
            }
        } else if (uses > 2) {
            ++r.nonmanifold_edges;
        }
        i = j;
    }
    return r;
}

}  // namespace sp08
