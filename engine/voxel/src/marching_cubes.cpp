// veach: canonical
// Spike SP-08 (VCH-084), throwaway: marching cubes with Transvoxel transition cells (Lengyel 2010,
// https://transvoxel.org/), on the vendored MIT tables in transvoxel_tables.inl
// (docs/research/spikes/sp-08-mesher.md).
//
// Regular cells use Lengyel's modified marching cubes tables. On each face whose neighbor is one level finer, a layer
// of transition cells joins this chunk's half-resolution face to the neighbor's full-resolution samples: the 9
// full-resolution samples of a transition cell are evaluated from the field on the face plane at half the spacing, so
// they are the neighbor's own boundary samples, and the vertices on the plane are computed with the same integer
// expression the neighbor uses. Regular vertices in the boundary cell layer are pushed inward by the transition width
// (Lengyel's shrink), except vertices on a face plane whose neighbor is not finer, which must stay where that
// neighbor puts them.

#include <veach/voxel/voxel.hpp>

#include <algorithm>
#include <cmath>

namespace veach::voxel {
namespace transvoxel {
#include "transvoxel_tables.inl"
}  // namespace transvoxel

namespace {

constexpr int lattice = chunk_cells + 1;           // lattice points per axis, 0..32
constexpr int face_lattice = 2 * chunk_cells + 1;  // full-resolution face points per axis, 0..64

// Lengyel's triangles are wound clockwise seen from outside the solid when bit i of the case is set for a negative
// corner; glTF and the renderer want counter-clockwise front faces. Checked by the winding test in
// tests/voxel_tests.cpp.
constexpr bool flip_regular = false;
constexpr bool flip_transition = false;

int corner_offset(int corner, int axis) {
    return (corner >> axis) & 1;
}

class TransvoxelMesher final : public IsoMesher {
public:
    explicit TransvoxelMesher(TransvoxelOptions options) : options_(options) {
        regular_edges_.resize(static_cast<std::size_t>(3 * lattice * lattice * lattice));
        if (options_.encoding != DistanceEncoding::linear) {
            decoded_.resize(65536);
            for (std::size_t i = 0; i < decoded_.size(); ++i) {
                decoded_[i] =
                    decode_distance(static_cast<std::int16_t>(static_cast<int>(i) - 32768), options_.encoding);
            }
        }
        for (auto& f : face_edges_) {
            f.resize(static_cast<std::size_t>(2 * face_lattice * face_lattice));
        }
    }

    std::string_view name() const override {
        return options_.transitions ? "marching cubes + transvoxel" : "marching cubes (no transitions)";
    }

    std::uint8_t face_samples_needed(ChunkNeighbors neighbors) const override {
        return options_.transitions ? neighbors.finer : std::uint8_t{0};
    }

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
        mask_ = options_.transitions && samples.key.level > 0 ? neighbors.finer : std::uint8_t{0};
        mask_ = static_cast<std::uint8_t>(mask_ & samples.face_mask);
        width_ = cell_ * options_.width_256 / 256;
        project_ = options_.project;
        std::fill(regular_edges_.begin(), regular_edges_.end(), -1);

        regular_cells();
        for (int f = 0; f < 6; ++f) {
            if (mask_ & (1u << f)) {
                transition_face(f);
            }
        }
    }

private:
    std::int16_t d(int x, int y, int z) const { return s_->at(x, y, z).distance; }

    /// What interpolation and gradients read: the code itself (linear, exact integers as SP-08 committed), or the
    /// decoded distance.
    double value(std::int16_t code) const {
        return decoded_.empty() ? static_cast<double>(code) : decoded_[static_cast<std::size_t>(code + 32768)];
    }
    double v(int x, int y, int z) const { return value(d(x, y, z)); }

    /// Offset of the crossing along an edge of length `len` (fixed point) from the sample with code c0.
    std::int64_t crossing(std::int16_t c0, std::int16_t c1, std::int64_t len) const {
        if (decoded_.empty()) {
            return std::int64_t{c0} * len / (std::int64_t{c0} - c1);
        }
        const double v0 = value(c0), v1 = value(c1);
        const auto off = static_cast<std::int64_t>(std::floor(v0 / (v0 - v1) * static_cast<double>(len) + 0.5));
        return std::clamp<std::int64_t>(off, 0, len);
    }

    std::uint32_t emit(const std::array<std::int64_t, 3>& local, const std::array<float, 3>& n, std::uint8_t material) {
        const auto index = static_cast<std::uint32_t>(out_->fixed.size());
        std::array<std::int64_t, 3> global{};
        std::array<float, 3> meters{};
        for (std::size_t a = 0; a < 3; ++a) {
            global[a] = (origin_[a] << fixed_bits) + local[a];
            meters[a] = static_cast<float>(static_cast<double>(local[a]) * (lod0_cell_meters / fixed_one));
        }
        out_->fixed.push_back(global);
        out_->position.push_back(meters);
        out_->normal.push_back(n);
        out_->material.push_back(material);
        return index;
    }

    static std::array<float, 3> normal(const std::array<double, 3>& g0, const std::array<double, 3>& g1,
                                       std::int64_t off, std::int64_t len) {
        const float t = static_cast<float>(off) / static_cast<float>(len);
        std::array<float, 3> n{};
        for (std::size_t a = 0; a < 3; ++a) {
            const float a0 = static_cast<float>(g0[a]);
            n[a] = a0 + t * (static_cast<float>(g1[a]) - a0);
        }
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0.0f) {
            for (float& c : n) {
                c /= l;
            }
        } else {
            n = {0.0f, 1.0f, 0.0f};
        }
        return n;
    }

    std::array<double, 3> gradient(int x, int y, int z) const {
        return {v(x + 1, y, z) - v(x - 1, y, z), v(x, y + 1, z) - v(x, y - 1, z), v(x, y, z + 1) - v(x, y, z - 1)};
    }

    /// Lengyel's shrink: moves a vertex of the boundary cell layer inward on faces with transition cells, projected
    /// on the surface's tangent plane (Lengyel 2010, section 4.4.2) so it slides along the surface instead of folding
    /// it.
    void displace(std::array<std::int64_t, 3>& q, const std::array<float, 3>& n) const {
        if (!mask_ || width_ == 0) {
            return;
        }
        const std::int64_t end = chunk_cells * cell_;
        for (std::size_t a = 0; a < 3; ++a) {
            const unsigned lo = 1u << (2 * a), hi = 1u << (2 * a + 1);
            if ((q[a] == 0 && !(mask_ & lo)) || (q[a] == end && !(mask_ & hi))) {
                return;  // on a face plane shared with a same-level or coarser neighbor: never moves
            }
        }
        std::array<double, 3> delta{};
        for (std::size_t a = 0; a < 3; ++a) {
            const unsigned lo = 1u << (2 * a), hi = 1u << (2 * a + 1);
            std::int64_t moved = q[a];
            if ((mask_ & lo) && q[a] < cell_) {
                moved = width_ + q[a] * (cell_ - width_) / cell_;
            } else if ((mask_ & hi) && q[a] > end - cell_) {
                moved = end - (width_ + (end - q[a]) * (cell_ - width_) / cell_);
            }
            delta[a] = static_cast<double>(moved - q[a]);
        }
        if (project_) {
            const double dn = delta[0] * n[0] + delta[1] * n[1] + delta[2] * n[2];
            for (std::size_t a = 0; a < 3; ++a) {
                delta[a] -= dn * static_cast<double>(n[a]);
            }
        }
        for (std::size_t a = 0; a < 3; ++a) {
            q[a] += static_cast<std::int64_t>(std::floor(delta[a] + 0.5));
        }
    }

    /// The vertex on the lattice edge from (x, y, z) to the next point along `axis`.
    std::uint32_t regular_vertex(int x, int y, int z, int axis) {
        const auto slot =
            static_cast<std::size_t>(axis * lattice * lattice * lattice + x + lattice * (y + lattice * z));
        if (regular_edges_[slot] >= 0) {
            return static_cast<std::uint32_t>(regular_edges_[slot]);
        }
        const int x1 = x + (axis == 0), y1 = y + (axis == 1), z1 = z + (axis == 2);
        const Sample& s0 = s_->at(x, y, z);
        const Sample& s1 = s_->at(x1, y1, z1);
        const std::int64_t off = crossing(s0.distance, s1.distance, cell_);
        std::array<std::int64_t, 3> q{x * cell_, y * cell_, z * cell_};
        q[static_cast<std::size_t>(axis)] += off;
        const auto n = normal(gradient(x, y, z), gradient(x1, y1, z1), off, cell_);
        displace(q, n);
        const std::uint32_t index = emit(q, n, s0.distance < 0 ? s0.material : s1.material);
        regular_edges_[slot] = static_cast<std::int32_t>(index);
        return index;
    }

    void triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c, bool flip, bool transition) {
        const auto& f = out_->fixed;
        if (f[a] == f[b] || f[b] == f[c] || f[a] == f[c]) {
            return;  // degenerate (a sample exactly on the surface)
        }
        if (flip) {
            std::swap(b, c);
        }
        out_->indices.insert(out_->indices.end(), {a, b, c});
        if (transition) {
            ++out_->transition_triangles;
        }
    }

    void regular_cells() {
        using namespace transvoxel;
        for (int z = 0; z < chunk_cells; ++z) {
            for (int y = 0; y < chunk_cells; ++y) {
                for (int x = 0; x < chunk_cells; ++x) {
                    unsigned code = 0;
                    for (int c = 0; c < 8; ++c) {
                        if (d(x + corner_offset(c, 0), y + corner_offset(c, 1), z + corner_offset(c, 2)) < 0) {
                            code |= 1u << c;
                        }
                    }
                    if (code == 0 || code == 255) {
                        continue;
                    }
                    const RegularCellData& cell = regularCellData[regularCellClass[code]];
                    std::array<std::uint32_t, 12> ids{};
                    const auto vertex_count = static_cast<std::size_t>(cell.GetVertexCount());
                    for (std::size_t k = 0; k < vertex_count; ++k) {
                        const unsigned edge = regularVertexData[code][k] & 0xFFu;
                        const int c0 = static_cast<int>(edge >> 4), c1 = static_cast<int>(edge & 0xFu);
                        const int axis = (c0 ^ c1) == 1 ? 0 : ((c0 ^ c1) == 2 ? 1 : 2);
                        const int lo = std::min(c0, c1);
                        ids[k] = regular_vertex(x + corner_offset(lo, 0), y + corner_offset(lo, 1),
                                                z + corner_offset(lo, 2), axis);
                    }
                    const auto triangle_count = static_cast<std::size_t>(cell.GetTriangleCount());
                    for (std::size_t t = 0; t < triangle_count; ++t) {
                        triangle(ids[cell.vertexIndex[3 * t]], ids[cell.vertexIndex[3 * t + 1]],
                                 ids[cell.vertexIndex[3 * t + 2]], flip_regular, false);
                    }
                }
            }
        }
    }

    /// The vertex on the full-resolution face edge from (u, v) to the next point along u (dir 0) or v (dir 1).
    std::uint32_t full_vertex(int f, int u, int v, int dir) {
        auto& slots = face_edges_[static_cast<std::size_t>(f)];
        const auto slot = static_cast<std::size_t>(dir * face_lattice * face_lattice + u + face_lattice * v);
        if (slots[slot] >= 0) {
            return static_cast<std::uint32_t>(slots[slot]);
        }
        const int a = f / 2, b = (a + 1) % 3, c = (a + 2) % 3;
        const int u1 = u + (dir == 0), v1 = v + (dir == 1);
        const Sample& s0 = s_->face_at(f, u, v, 0);
        const Sample& s1 = s_->face_at(f, u1, v1, 0);
        const std::int64_t half = cell_ / 2;
        const std::int64_t off = crossing(s0.distance, s1.distance, half);
        std::array<std::int64_t, 3> q{};
        q[static_cast<std::size_t>(a)] = (f & 1) ? chunk_cells * cell_ : 0;
        q[static_cast<std::size_t>(b)] = u * half + (dir == 0 ? off : 0);
        q[static_cast<std::size_t>(c)] = v * half + (dir == 1 ? off : 0);
        const auto grad = [&](int pu, int pv) {
            const auto at = [&](int du, int dv, int layer) {
                return value(s_->face_at(f, du, dv, layer).distance);
            };
            std::array<double, 3> g{};
            g[static_cast<std::size_t>(a)] = at(pu, pv, 1) - at(pu, pv, -1);
            g[static_cast<std::size_t>(b)] = at(pu + 1, pv, 0) - at(pu - 1, pv, 0);
            g[static_cast<std::size_t>(c)] = at(pu, pv + 1, 0) - at(pu, pv - 1, 0);
            return g;
        };
        const auto n = normal(grad(u, v), grad(u1, v1), off, half);
        const std::uint32_t index = emit(q, n, s0.distance < 0 ? s0.material : s1.material);
        slots[slot] = static_cast<std::int32_t>(index);
        return index;
    }

    void transition_face(int f) {
        using namespace transvoxel;
        auto& slots = face_edges_[static_cast<std::size_t>(f)];
        std::fill(slots.begin(), slots.end(), -1);
        const int a = f / 2, b = (a + 1) % 3, c = (a + 2) % 3;
        const int plane = (f & 1) ? chunk_cells : 0;
        // Case bits in Lengyel's order: samples 0, 1, 2, 5, 8, 7, 6, 3, 4 (row-major 3 × 3, u fastest).
        constexpr std::array<int, 9> bit_of_sample{0, 1, 2, 7, 8, 3, 6, 5, 4};
        // The (u, v) orientation of the face (u × v = +axis) is mirrored on maximum faces, seen from inside.
        const bool mirrored = (f & 1) != 0;
        for (int j = 0; j < chunk_cells; ++j) {
            for (int i = 0; i < chunk_cells; ++i) {
                unsigned code = 0;
                for (int n = 0; n < 9; ++n) {
                    if (s_->face_at(f, 2 * i + n % 3, 2 * j + n / 3, 0).distance < 0) {
                        code |= 1u << bit_of_sample[static_cast<std::size_t>(n)];
                    }
                }
                if (code == 0 || code == 511) {
                    continue;
                }
                const unsigned cls = transitionCellClass[code];
                const TransitionCellData& cell = transitionCellData[cls & 0x7Fu];
                const bool inverted = (cls & 0x80u) != 0;
                std::array<std::uint32_t, 12> ids{};
                const auto vertex_count = static_cast<std::size_t>(cell.GetVertexCount());
                for (std::size_t k = 0; k < vertex_count; ++k) {
                    const unsigned edge = transitionVertexData[code][k] & 0xFFu;
                    int e0 = static_cast<int>(edge >> 4), e1 = static_cast<int>(edge & 0xFu);
                    if (e0 > e1) {
                        std::swap(e0, e1);
                    }
                    if (e0 >= 9) {
                        // Half-resolution face: a regular lattice edge of this chunk's face (9, A, B, C are the
                        // corners (0, 0), (1, 0), (0, 1), (1, 1) of the coarse cell).
                        const int du0 = (e0 - 9) & 1, dv0 = (e0 - 9) >> 1;
                        const int du1 = (e1 - 9) & 1, dv1 = (e1 - 9) >> 1;
                        std::array<int, 3> p{};
                        p[static_cast<std::size_t>(a)] = plane;
                        p[static_cast<std::size_t>(b)] = i + std::min(du0, du1);
                        p[static_cast<std::size_t>(c)] = j + std::min(dv0, dv1);
                        ids[k] = regular_vertex(p[0], p[1], p[2], du0 != du1 ? b : c);
                    } else {
                        const int u0 = 2 * i + e0 % 3, v0 = 2 * j + e0 / 3;
                        const int u1 = 2 * i + e1 % 3;
                        ids[k] = full_vertex(f, u0, v0, u0 != u1 ? 0 : 1);
                    }
                }
                const bool flip = flip_transition != (inverted != mirrored);
                const auto triangle_count = static_cast<std::size_t>(cell.GetTriangleCount());
                for (std::size_t t = 0; t < triangle_count; ++t) {
                    triangle(ids[cell.vertexIndex[3 * t]], ids[cell.vertexIndex[3 * t + 1]],
                             ids[cell.vertexIndex[3 * t + 2]], flip, true);
                }
            }
        }
    }

    TransvoxelOptions options_;
    std::vector<double> decoded_;  ///< companded: distance by code + 32768; empty for linear
    std::vector<std::int32_t> regular_edges_;
    std::array<std::vector<std::int32_t>, 6> face_edges_;
    const ChunkSamples* s_ = nullptr;
    ChunkMesh* out_ = nullptr;
    std::array<std::int64_t, 3> origin_{};
    std::int64_t cell_ = 0;
    std::int64_t width_ = 0;
    std::uint8_t mask_ = 0;
    bool project_ = true;
};

}  // namespace

std::unique_ptr<IsoMesher> make_transvoxel_mesher(TransvoxelOptions options) {
    return std::make_unique<TransvoxelMesher>(options);
}

}  // namespace veach::voxel
