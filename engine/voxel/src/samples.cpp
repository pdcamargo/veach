// veach: canonical
// Spike SP-08 (VCH-084), throwaway: chunk sampling and mesh hashes (docs/research/spikes/sp-08-mesher.md).

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/voxel/voxel.hpp>

#include <algorithm>
#include <cmath>

namespace veach::voxel {

namespace {

/// Companded codes: |code| <= 16384 is |d| x 1024; above, 16384 + K ln(1 + (|d| - 16) / D) with D = K / 1024, K set so
/// that 10,000 km is 32767.
struct Compander {
    double k = 0.0;
    double d = 0.0;
    Compander() {
        double lo = 100.0, hi = 5000.0;
        for (int i = 0; i < 80; ++i) {
            const double mid = 0.5 * (lo + hi);
            (mid * dmath::log(1.0 + 1.0e7 / (mid / 1024.0)) < 16383.0 ? lo : hi) = mid;
        }
        k = lo;
        d = k / 1024.0;
    }
};

const Compander& compander() {
    static const Compander c;
    return c;
}

}  // namespace

std::int16_t encode_distance(double meters, DistanceEncoding encoding) {
    const double a = std::abs(meters);
    double code = 0.0;
    if (encoding == DistanceEncoding::linear || a <= 16.0) {
        code = std::floor(a * 1024.0 + 0.5);
    } else {
        const Compander& c = compander();
        code = 16384.0 + std::floor(c.k * dmath::log(1.0 + (a - 16.0) / c.d) + 0.5);
    }
    code = std::min(code, 32767.0);
    return static_cast<std::int16_t>(meters < 0.0 ? -code : code);
}

double decode_distance(std::int16_t code, DistanceEncoding encoding) {
    const double c = std::abs(static_cast<double>(code));
    double a = c / 1024.0;
    if (encoding == DistanceEncoding::companded && c > 16384.0) {
        const Compander& k = compander();
        a = 16.0 + k.d * (dmath::exp((c - 16384.0) / k.k) - 1.0);
    }
    return code < 0 ? -a : a;
}

void sample_chunk(const DensityField& field, const ChunkKey& key, std::uint8_t face_mask, ChunkSamples& out) {
    constexpr int n = ChunkSamples::grid_n;
    constexpr int fn = ChunkSamples::face_n;
    out.key = key;
    out.face_mask = face_mask;
    out.grid.resize(static_cast<std::size_t>(n) * n * n);
    const auto o = chunk_origin(key);
    const std::int64_t s = cell_units(key.level);
    bool neg = false, pos = false;
    std::size_t i = 0;
    for (int z = -1; z <= chunk_cells + 1; ++z) {
        for (int y = -1; y <= chunk_cells + 1; ++y) {
            for (int x = -1; x <= chunk_cells + 1; ++x) {
                const Sample v = field.sample(o[0] + x * s, o[1] + y * s, o[2] + z * s);
                out.grid[i++] = v;
                (v.distance < 0 ? neg : pos) = true;
            }
        }
    }
    for (int f = 0; f < 6; ++f) {
        auto& face = out.faces[static_cast<std::size_t>(f)];
        if (!(face_mask & (1u << f)) || key.level == 0) {
            face.clear();
            continue;
        }
        face.resize(static_cast<std::size_t>(fn) * fn * 3);
        const int a = f / 2, b = (a + 1) % 3, c = (a + 2) % 3;
        const std::int64_t h = s / 2;
        const std::int64_t plane = o[static_cast<std::size_t>(a)] + ((f & 1) ? chunk_units(key.level) : 0);
        std::size_t j = 0;
        for (int layer = -1; layer <= 1; ++layer) {
            for (int v = -1; v <= 2 * chunk_cells + 1; ++v) {
                for (int u = -1; u <= 2 * chunk_cells + 1; ++u) {
                    std::array<std::int64_t, 3> p{};
                    p[static_cast<std::size_t>(a)] = plane + layer * h;
                    p[static_cast<std::size_t>(b)] = o[static_cast<std::size_t>(b)] + u * h;
                    p[static_cast<std::size_t>(c)] = o[static_cast<std::size_t>(c)] + v * h;
                    const Sample sv = field.sample(p[0], p[1], p[2]);
                    face[j++] = sv;
                    if (layer == 0) {
                        (sv.distance < 0 ? neg : pos) = true;
                    }
                }
            }
        }
    }
    out.has_surface = neg && pos;
}

void ChunkMesh::clear() {
    fixed.clear();
    position.clear();
    normal.clear();
    material.clear();
    indices.clear();
    transition_triangles = 0;
    skirt_triangles = 0;
}

namespace {
std::uint64_t absorb(std::uint64_t h, std::uint64_t w) {
    return mix64(h ^ mix64(w + hash_golden));
}
std::uint64_t quantize(float n) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(n * 32767.0f + 0.5f)));
}
}  // namespace

std::uint64_t mesh_hash(const ChunkMesh& m) {
    std::uint64_t h = hash(hash_id("veach:voxel.mesh"), m.key.level, static_cast<std::uint32_t>(m.key.x),
                           static_cast<std::uint32_t>(m.key.y), static_cast<std::uint32_t>(m.key.z));
    h = absorb(h, m.fixed.size());
    for (std::size_t i = 0; i < m.fixed.size(); ++i) {
        for (std::size_t a = 0; a < 3; ++a) {
            h = absorb(h, static_cast<std::uint64_t>(m.fixed[i][a]));
            h = absorb(h, quantize(m.normal[i][a]));
        }
        h = absorb(h, m.material[i]);
    }
    h = absorb(h, m.indices.size());
    for (const std::uint32_t index : m.indices) {
        h = absorb(h, index);
    }
    return h;
}

std::uint64_t combine_hashes(const std::vector<std::uint64_t>& hashes) {
    std::uint64_t h = hash_id("veach:voxel.meshes");
    for (const std::uint64_t c : hashes) {
        h = absorb(h, c);
    }
    return h;
}

}  // namespace veach::voxel
