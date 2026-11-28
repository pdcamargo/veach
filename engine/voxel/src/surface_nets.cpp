// veach: canonical
// Spike SP-08 (VCH-084), throwaway: naive surface nets (one vertex per cell at the mean of its edge crossings, one
// quad per crossing lattice edge, https://0fps.net/2012/07/12/smooth-voxel-terrain-part-2/) with skirts at LOD seams
// (docs/research/spikes/sp-08-mesher.md).
//
// Ownership: a chunk emits the quads of the lattice edges whose lower point is in [0, 32)³, using cell vertices from
// -1 to 31; cell -1 comes from the apron, so it is the same integer vertex the neighbor computes for its cell 31 and
// same-level seams are closed. On faces touching another level the chunk also emits the edges on its maximum face.
// Across a level change the two sides do not meet: each face that touches a chunk of another level (through the face,
// or diagonally through an edge or corner) gets a skirt, the chunk's open boundary edges extruded into the solid along
// the normal.

#include <veach/voxel/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace veach::voxel {
namespace {

constexpr int cells = chunk_cells + 2;  // cells -1..32 per axis (32 only on seam faces)

class SurfaceNetsMesher final : public IsoMesher {
public:
    explicit SurfaceNetsMesher(SurfaceNetsOptions options) : options_(options) {
        cell_vertex_.resize(static_cast<std::size_t>(cells * cells * cells));
    }

    std::string_view name() const override { return options_.skirts ? "surface nets + skirts" : "surface nets"; }

    std::uint8_t face_samples_needed(ChunkNeighbors) const override { return 0; }

    void mesh(const ChunkSamples& samples, ChunkNeighbors neighbors, ChunkMesh& out) override {
        out.clear();
        out.key = samples.key;
        if (!samples.has_surface) {
            return;
        }
        s_ = &samples;
        out_ = &out;
        origin_ = chunk_origin(samples.key);
        cell_ = cell_units(samples.key.level) << fixed_bits;
        std::fill(cell_vertex_.begin(), cell_vertex_.end(), -1);
        vertex_cell_.clear();
        // On seam faces the mesh reaches half a cell past the face on both sides (cell -1 on minimum faces, cell 32
        // on maximum ones), so the two levels overlap instead of leaving a gap the skirts can't cover from above.
        const auto seams = options_.skirts
                               ? static_cast<std::uint8_t>(neighbors.finer | neighbors.coarser | neighbors.touching)
                               : std::uint8_t{0};
        const auto level_faces = static_cast<std::uint8_t>(seams & (neighbors.finer | neighbors.coarser));
        for (std::size_t a = 0; a < 3; ++a) {
            last_[a] = (level_faces & (1u << (2 * a + 1))) ? chunk_cells : chunk_cells - 1;
        }

        for (int z = -1; z <= last_[2]; ++z) {
            for (int y = -1; y <= last_[1]; ++y) {
                for (int x = -1; x <= last_[0]; ++x) {
                    cell(x, y, z);
                }
            }
        }
        quads();
        if (seams) {
            skirts(seams);
        }
    }

private:
    std::int16_t d(int x, int y, int z) const { return s_->at(x, y, z).distance; }

    static std::size_t slot(int x, int y, int z) {
        return static_cast<std::size_t>((x + 1) + cells * ((y + 1) + cells * (z + 1)));
    }

    std::uint32_t emit(const std::array<std::int64_t, 3>& global, const std::array<float, 3>& n,
                       std::uint8_t material) {
        const auto index = static_cast<std::uint32_t>(out_->fixed.size());
        std::array<float, 3> meters{};
        for (std::size_t a = 0; a < 3; ++a) {
            const std::int64_t local = global[a] - (origin_[a] << fixed_bits);
            meters[a] = static_cast<float>(static_cast<double>(local) * (lod0_cell_meters / fixed_one));
        }
        out_->fixed.push_back(global);
        out_->position.push_back(meters);
        out_->normal.push_back(n);
        out_->material.push_back(material);
        return index;
    }

    void cell(int x, int y, int z) {
        std::array<std::int16_t, 8> c{};
        unsigned code = 0;
        for (int i = 0; i < 8; ++i) {
            c[static_cast<std::size_t>(i)] = d(x + (i & 1), y + ((i >> 1) & 1), z + ((i >> 2) & 1));
            if (c[static_cast<std::size_t>(i)] < 0) {
                code |= 1u << i;
            }
        }
        if (code == 0 || code == 255) {
            return;
        }
        std::array<std::int64_t, 3> sum{};
        std::int64_t count = 0;
        std::array<int, 3> g{};
        int deepest = 0;
        for (int i = 0; i < 8; ++i) {
            if (c[static_cast<std::size_t>(i)] < c[static_cast<std::size_t>(deepest)]) {
                deepest = i;
            }
            for (int axis = 0; axis < 3; ++axis) {
                const int j = i | (1 << axis);
                if (j == i) {
                    continue;
                }
                const std::int64_t d0 = c[static_cast<std::size_t>(i)], d1 = c[static_cast<std::size_t>(j)];
                g[static_cast<std::size_t>(axis)] += static_cast<int>(d1 - d0);
                if ((d0 < 0) == (d1 < 0)) {
                    continue;
                }
                const std::int64_t off = d0 * cell_ / (d0 - d1);
                for (std::size_t a = 0; a < 3; ++a) {
                    const int p = (a == 0 ? x : (a == 1 ? y : z)) + ((i >> a) & 1);
                    sum[a] += ((origin_[a] + p * cell_units(s_->key.level)) << fixed_bits) +
                              (static_cast<int>(a) == axis ? off : 0);
                }
                ++count;
            }
        }
        std::array<std::int64_t, 3> fixed{};
        for (std::size_t a = 0; a < 3; ++a) {
            fixed[a] = sum[a] / count;
        }
        std::array<float, 3> n{static_cast<float>(g[0]), static_cast<float>(g[1]), static_cast<float>(g[2])};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0.0f) {
            for (float& v : n) {
                v /= l;
            }
        } else {
            n = {0.0f, 1.0f, 0.0f};
        }
        const int dx = deepest & 1, dy = (deepest >> 1) & 1, dz = (deepest >> 2) & 1;
        const std::uint32_t index = emit(fixed, n, s_->at(x + dx, y + dy, z + dz).material);
        cell_vertex_[slot(x, y, z)] = static_cast<std::int32_t>(index);
        vertex_cell_.push_back({x, y, z});
    }

    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const auto& f = out_->fixed;
        if (f[a] == f[b] || f[b] == f[c] || f[a] == f[c]) {
            return;
        }
        out_->indices.insert(out_->indices.end(), {a, b, c});
    }

    void quads() {
        for (int axis = 0; axis < 3; ++axis) {
            const int b = (axis + 1) % 3, c = (axis + 2) % 3;
            for (int z = 0; z <= last_[2]; ++z) {
                for (int y = 0; y <= last_[1]; ++y) {
                    for (int x = 0; x <= last_[0]; ++x) {
                        const std::array<int, 3> p{x, y, z};
                        std::array<int, 3> q = p;
                        ++q[static_cast<std::size_t>(axis)];
                        const bool in0 = d(p[0], p[1], p[2]) < 0;
                        if (in0 == (d(q[0], q[1], q[2]) < 0)) {
                            continue;
                        }
                        const auto at = [&](int db, int dc) {
                            std::array<int, 3> r = p;
                            r[static_cast<std::size_t>(b)] += db;
                            r[static_cast<std::size_t>(c)] += dc;
                            return static_cast<std::uint32_t>(cell_vertex_[slot(r[0], r[1], r[2])]);
                        };
                        // Counter-clockwise in (b, c), b × c = axis: faces +axis, out of the solid when p is inside.
                        const std::uint32_t va = at(-1, -1), vb = at(0, -1), vc = at(0, 0), vd = at(-1, 0);
                        if (in0) {
                            triangle(va, vb, vc);
                            triangle(va, vc, vd);
                        } else {
                            triangle(va, vc, vb);
                            triangle(va, vd, vc);
                        }
                    }
                }
            }
        }
    }

    void skirts(std::uint8_t seams) {
        // Open edges: undirected edges used by exactly one triangle, kept with their direction in that triangle.
        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> edges;
        const auto& idx = out_->indices;
        edges.reserve(idx.size());
        for (std::size_t t = 0; t < idx.size(); t += 3) {
            for (std::size_t e = 0; e < 3; ++e) {
                const std::uint32_t u = idx[t + e], v = idx[t + (e + 1) % 3];
                edges.emplace_back(std::min(u, v), std::max(u, v), u, v);
            }
        }
        std::sort(edges.begin(), edges.end());
        const std::int64_t length = cell_ * options_.skirt_256 / 256;
        std::vector<std::int32_t> bottom(out_->fixed.size(), -1);
        const auto lower = [&](std::uint32_t v) {
            if (bottom[v] < 0) {
                std::array<std::int64_t, 3> p = out_->fixed[v];
                const std::array<float, 3> n = out_->normal[v];
                for (std::size_t a = 0; a < 3; ++a) {
                    p[a] -= static_cast<std::int64_t>(
                        std::floor(static_cast<double>(n[a]) * static_cast<double>(length) + 0.5));
                }
                bottom[v] = static_cast<std::int32_t>(emit(p, n, out_->material[v]));
            }
            return static_cast<std::uint32_t>(bottom[v]);
        };
        const std::size_t triangles_before = idx.size() / 3;
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i + 1;
            while (j < edges.size() && std::get<0>(edges[j]) == std::get<0>(edges[i]) &&
                   std::get<1>(edges[j]) == std::get<1>(edges[i])) {
                ++j;
            }
            if (j - i == 1) {
                const std::uint32_t u = std::get<2>(edges[i]), v = std::get<3>(edges[i]);
                const auto& cu = vertex_cell_[u];
                const auto& cv = vertex_cell_[v];
                // Both ends in the boundary cell layer of a seam face (not necessarily the same one: edges that turn a
                // chunk corner need skirts too).
                const auto on_seam = [&](const std::array<int, 3>& cell) {
                    for (int f = 0; f < 6; ++f) {
                        const auto a = static_cast<std::size_t>(f / 2);
                        if ((seams & (1u << f)) && cell[a] == ((f & 1) ? last_[a] : -1)) {
                            return true;
                        }
                    }
                    return false;
                };
                if (on_seam(cu) && on_seam(cv)) {
                    const std::uint32_t ub = lower(u), vb = lower(v);
                    triangle(v, u, ub);
                    triangle(v, ub, vb);
                }
            }
            i = j;
        }
        out_->skirt_triangles = static_cast<std::uint32_t>(idx.size() / 3 - triangles_before);
    }

    SurfaceNetsOptions options_;
    std::vector<std::int32_t> cell_vertex_;
    std::vector<std::array<int, 3>> vertex_cell_;  ///< cell of each surface vertex (skirt vertices are appended after)
    const ChunkSamples* s_ = nullptr;
    ChunkMesh* out_ = nullptr;
    std::array<std::int64_t, 3> origin_{};
    std::int64_t cell_ = 0;
    std::array<int, 3> last_{};  ///< last cell (and lattice edge start) per axis: 31, or 32 on a seam face
};

}  // namespace

std::unique_ptr<IsoMesher> make_surface_nets_mesher(SurfaceNetsOptions options) {
    return std::make_unique<SurfaceNetsMesher>(options);
}

}  // namespace veach::voxel
