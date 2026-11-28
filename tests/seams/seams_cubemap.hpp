// Seams sweep, throwaway: are the procgen inputs that ARE cube-mapped continuous across cube-face edges
// (docs/research/spikes/seams-sweep.md)? SP-07's climate and biome map (64² × 6 cells, its Luau hook) and SP-06's
// field graph evaluated over face regions (engine/procgen since VCH-112). Header-only: the bench and veach-tests
// include it (both link SP-06, the procgen sandbox and space gen).
#pragma once

#include <veach/procgen/field_graph.hpp>

#include "support/sp07_climate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace veach::seams::cubemap {

/// SP-07's cube-map layout: face f has center c, and (u, v) in [-1, 1] along axes eu, ev (d = c + u eu + v ev).
struct Face {
    std::array<double, 3> c, eu, ev;
};
inline constexpr std::array<Face, 6> faces{{
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
    {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
    {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
}};

struct Cell {
    int face = 0, i = 0, j = 0;
};

inline std::array<double, 3> direction(int face, double u, double v) {
    const Face& f = faces[static_cast<std::size_t>(face)];
    std::array<double, 3> d{};
    for (std::size_t a = 0; a < 3; ++a) {
        d[a] = f.c[a] + u * f.eu[a] + v * f.ev[a];
    }
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    return {d[0] / l, d[1] / l, d[2] / l};
}

/// The cell holding direction `d` on an n × n × 6 map.
inline Cell cell_of(const std::array<double, 3>& d, int n) {
    int best = 0;
    double best_dot = -2.0;
    for (int f = 0; f < 6; ++f) {
        const Face& face = faces[static_cast<std::size_t>(f)];
        const double dot = d[0] * face.c[0] + d[1] * face.c[1] + d[2] * face.c[2];
        if (dot > best_dot) {
            best_dot = dot;
            best = f;
        }
    }
    const Face& face = faces[static_cast<std::size_t>(best)];
    const double u = (d[0] * face.eu[0] + d[1] * face.eu[1] + d[2] * face.eu[2]) / best_dot;
    const double v = (d[0] * face.ev[0] + d[1] * face.ev[1] + d[2] * face.ev[2]) / best_dot;
    const auto index = [n](double t) {
        return std::clamp(static_cast<int>(std::floor((t + 1.0) * 0.5 * n)), 0, n - 1);
    };
    return {best, index(u), index(v)};
}

inline std::size_t index_of(const Cell& c, int n) {
    return (static_cast<std::size_t>(c.face) * static_cast<std::size_t>(n) + static_cast<std::size_t>(c.j)) *
               static_cast<std::size_t>(n) +
           static_cast<std::size_t>(c.i);
}

/// Neighboring cell pairs: each cell with the cell one step along +u and +v (on its face, or past the edge onto the
/// next face: the center one cell further along the face plane, projected).
struct Pair {
    std::size_t a, b;
    double angle;  ///< between the two cells' centers, radians
    bool across;   ///< on different faces
    bool edge;     ///< within a face, and both cells in the last two rows or columns before an edge
};

inline std::vector<Pair> neighbor_pairs(int n) {
    std::vector<Pair> pairs;
    const auto center = [n](int face, double i, double j) {
        return direction(face, 2.0 * (i + 0.5) / n - 1.0, 2.0 * (j + 0.5) / n - 1.0);
    };
    for (int f = 0; f < 6; ++f) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const auto d = center(f, i, j);
                for (const auto& [di, dj] : {std::pair{1, 0}, std::pair{0, 1}}) {
                    const auto e = center(f, i + di, j + dj);
                    const Cell other = cell_of(e, n);
                    const auto o = center(other.face, other.i, other.j);
                    const double dot = std::clamp(d[0] * o[0] + d[1] * o[1] + d[2] * o[2], -1.0, 1.0);
                    const auto near = [n](int k) {
                        return k <= 1 || k >= n - 2;
                    };
                    const bool edge = other.face == f &&
                                      ((di == 1 && (near(i) && near(i + 1))) || (dj == 1 && (near(j) && near(j + 1))));
                    pairs.push_back(
                        {index_of({f, i, j}, n), index_of(other, n), std::acos(dot), other.face != f, edge});
                }
            }
        }
    }
    return pairs;
}

/// |Δvalue| / angle over neighbor pairs, within a face and across a face edge. `edge_only`: the within-face pairs
/// are only those next to an edge (cells there are as small as the pairs across it, half the size of central ones).
struct Gradient {
    std::size_t within = 0, across = 0;
    double within_p99 = 0, within_max = 0, across_p99 = 0, across_max = 0;
    double within_angle = 0, across_angle = 0;      ///< mean spacing, radians
    double within_changed = 0, across_changed = 0;  ///< fraction of pairs whose values differ (categorical data)
};

template <typename T>
Gradient gradient(std::span<const T> values, const std::vector<Pair>& pairs, bool edge_only = true) {
    std::vector<double> w, x;
    Gradient g;
    std::size_t wc = 0, xc = 0;
    for (const Pair& p : pairs) {
        if (edge_only && !p.across && !p.edge) {
            continue;
        }
        const double dv = std::abs(static_cast<double>(values[p.a]) - static_cast<double>(values[p.b]));
        (p.across ? x : w).push_back(dv / p.angle);
        (p.across ? g.across_angle : g.within_angle) += p.angle;
        (p.across ? xc : wc) += dv != 0.0 ? 1 : 0;
    }
    const auto q = [](std::vector<double>& v, double f) {
        if (v.empty()) {
            return 0.0;
        }
        const auto k = static_cast<std::size_t>(f * static_cast<double>(v.size() - 1));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return v[k];
    };
    g.within = w.size();
    g.across = x.size();
    g.within_angle /= static_cast<double>(std::max<std::size_t>(g.within, 1));
    g.across_angle /= static_cast<double>(std::max<std::size_t>(g.across, 1));
    g.within_changed = static_cast<double>(wc) / static_cast<double>(std::max<std::size_t>(g.within, 1));
    g.across_changed = static_cast<double>(xc) / static_cast<double>(std::max<std::size_t>(g.across, 1));
    g.within_max = w.empty() ? 0.0 : *std::max_element(w.begin(), w.end());
    g.across_max = x.empty() ? 0.0 : *std::max_element(x.begin(), x.end());
    g.within_p99 = q(w, 0.99);
    g.across_p99 = q(x, 0.99);
    return g;
}

/// SP-06's FaceRegion for SP-07's face f at `n`²: sample (i, j) is SP-07's cell (f, i, j) (FaceRegion's v runs down).
inline procgen::FaceRegion face_region(int f, int n) {
    const Face& face = faces[static_cast<std::size_t>(f)];
    procgen::FaceRegion r;
    for (std::size_t a = 0; a < 3; ++a) {
        r.center[a] = static_cast<float>(face.c[a]);
        r.right[a] = static_cast<float>(face.eu[a]);
        r.up[a] = static_cast<float>(-face.ev[a]);
    }
    r.resolution = static_cast<std::uint32_t>(n);
    return r;
}

/// SP-06's terrain graph over the six faces at n², in SP-07's cell order.
inline std::vector<float> field_graph_map(const procgen::FieldProgram& program, int n) {
    std::vector<float> out(static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * 6);
    for (int f = 0; f < 6; ++f) {
        procgen::evaluate(program, face_region(f, n),
                          std::span<float>(out).subspan(
                              static_cast<std::size_t>(f) * static_cast<std::size_t>(n) * static_cast<std::size_t>(n),
                              static_cast<std::size_t>(n) * static_cast<std::size_t>(n)));
    }
    return out;
}

}  // namespace veach::seams::cubemap
