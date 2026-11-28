// Spike SP-08 (VCH-084), throwaway: the two meshers on a small piece of the test sphere, nine 64 m roots under the
// focus near its top, with LOD 0, 1 and 2 touching (docs/research/spikes/sp-08-mesher.md). The committed hashes are
// checked by the macOS gate and Windows CI: the meshers are canonical code (ADR 0043).

#include "support/sp08_field.hpp"

#include <doctest/doctest.h>

#include <functional>
#include <map>

namespace {

using namespace sp08;

LodSettings small_region() {
    LodSettings s;
    s.focus = {80.0, SphereField::radius / lod0_cell_meters, 80.0};  // 40 m from the top on x and z
    s.lambda = 0.2;
    s.root_level = 2;  // 64 m roots
    s.root_min = {-1, 4, -1};
    s.root_max = {1, 4, 1};
    return s;
}
constexpr std::array<std::int64_t, 3> region_lo{-128, 512, -128};
constexpr std::array<std::int64_t, 3> region_hi{256, 640, 256};

const std::vector<ChunkLeaf>& region_leaves() {
    static const std::vector<ChunkLeaf> leaves = select_chunks(small_region());
    return leaves;
}

/// Sampled once (sampling dominates in a Debug build), on 1 or 4 threads.
const std::vector<ChunkSamples>& region_samples(unsigned threads) {
    static const std::vector<ChunkSamples> one = sample_all(SphereField{}, region_leaves(), 1);
    static const std::vector<ChunkSamples> four = sample_all(SphereField{}, region_leaves(), 4);
    return threads == 1 ? one : four;
}

std::vector<MeshedChunk> mesh_region(const std::function<std::unique_ptr<IsoMesher>()>& make, unsigned threads) {
    return mesh_samples(region_samples(threads), region_leaves(), make, threads);
}

const auto transvoxel = [] {
    return make_transvoxel_mesher({});
};
const auto surface_nets = [] {
    return make_surface_nets_mesher({});
};

}  // namespace

TEST_CASE("[sp08] the octree is restricted, has three levels and symmetric neighbor masks") {
    const auto leaves = select_chunks(small_region());
    std::map<ChunkKey, ChunkNeighbors> by_key;
    std::array<int, 3> levels{};
    for (const auto& l : leaves) {
        by_key[l.key] = l.neighbors;
        REQUIRE(l.key.level <= 2);
        ++levels[l.key.level];
    }
    CHECK(levels[0] > 0);
    CHECK(levels[1] > 0);
    CHECK(levels[2] > 0);
    int seams = 0;
    for (const auto& l : leaves) {
        for (int f = 0; f < 6; ++f) {
            if (!(l.neighbors.finer & (1u << f))) {
                continue;
            }
            ++seams;
            // Every finer neighbor across face f sees this chunk as coarser across the opposite face.
            const int a = f / 2;
            const auto o = chunk_origin(l.key);
            const std::int64_t e = chunk_units(l.key.level), fe = e / 2;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    std::array<std::int64_t, 3> p{o[0], o[1], o[2]};
                    p[static_cast<std::size_t>((a + 1) % 3)] += i * fe;
                    p[static_cast<std::size_t>((a + 2) % 3)] += j * fe;
                    p[static_cast<std::size_t>(a)] += (f & 1) ? e : -fe;
                    const ChunkKey k{static_cast<std::uint8_t>(l.key.level - 1), static_cast<std::int32_t>(p[0] / fe),
                                     static_cast<std::int32_t>(p[1] / fe), static_cast<std::int32_t>(p[2] / fe)};
                    REQUIRE(by_key.contains(k));
                    CHECK((by_key[k].coarser & (1u << (f ^ 1))) != 0);
                }
            }
        }
    }
    CHECK(seams > 0);
}

TEST_CASE("[sp08] marching cubes + Transvoxel is watertight across LOD seams, plain marching cubes is not") {
    const auto tv = mesh_region(transvoxel, 1);
    const SeamReport r = check_seams(tv, region_lo, region_hi);
    std::size_t transition = 0;
    for (const auto& c : tv) {
        transition += c.mesh.transition_triangles;
    }
    CHECK(transition > 0);
    CHECK(r.triangles > 10000);
    CHECK(r.open_edges == 0);
    CHECK(r.nonmanifold_edges == 0);
    CHECK(r.boundary_edges > 0);  // the region is cut out of the body
    CHECK(static_cast<double>(r.wound_out) > 0.999 * static_cast<double>(r.triangles));

    const auto flat = mesh_region([] { return make_transvoxel_mesher({.width_256 = 0}); }, 1);
    CHECK(check_seams(flat, region_lo, region_hi).open_edges == 0);

    const auto mc = mesh_region([] { return make_transvoxel_mesher({.transitions = false}); }, 1);
    const SeamReport cracked = check_seams(mc, region_lo, region_hi);
    CHECK(cracked.open_edges > 100);
    CHECK(cracked.open_edges == cracked.open_at_level_change);
}

TEST_CASE("[sp08] surface nets closes same-level seams, only chunks touching another level are open") {
    const auto sn = mesh_region([] { return make_surface_nets_mesher({.skirts = false}); }, 1);
    const SeamReport r = check_seams(sn, region_lo, region_hi);
    CHECK(r.open_edges > 0);
    CHECK(r.open_edges == r.open_at_level_change);
    // Naive surface nets is not manifold where a cell holds two sheets (they share the cell's one vertex).
    MESSAGE("surface nets: ", r.nonmanifold_edges, " non-manifold edges of ", r.triangles, " triangles");
    CHECK(static_cast<double>(r.wound_out) > 0.999 * static_cast<double>(r.triangles));

    const auto skirted = mesh_region(surface_nets, 1);
    std::size_t skirts = 0;
    for (const auto& c : skirted) {
        skirts += c.mesh.skirt_triangles;
    }
    CHECK(skirts > 0);
}

TEST_CASE("[sp08] mesh hashes match the committed ones on 1 and 4 threads") {
    const auto tv1 = mesh_region(transvoxel, 1);
    const auto tv4 = mesh_region(transvoxel, 4);
    const auto sn1 = mesh_region(surface_nets, 1);
    const auto sn4 = mesh_region(surface_nets, 4);
    CHECK(meshes_hash(tv1) == meshes_hash(tv4));
    CHECK(meshes_hash(sn1) == meshes_hash(sn4));
    // Committed from macOS arm64; Windows x64 CI must agree. A change here is a change of mesher output: update the
    // constants on purpose and say why in the commit.
    CHECK(meshes_hash(tv1) == 0xbbc4053e4553b91dULL);
    CHECK(meshes_hash(sn1) == 0xec92ede1b61f4da6ULL);
}

// Spike SP-09 (VCH-085), throwaway: the companded distance encoding (docs/research/spikes/sp-09-descent.md). Coarse
// chunks have cells of up to 262 km; ADR 0053's linear codes saturate at 32 m, so their crossings fall at mid-edge and
// their gradients vanish. Companded codes are the same within 16 m and logarithmic beyond.

namespace {

/// A 20 km ball, sampled with either encoding: at levels 8 to 10 its cells are 128 to 512 m.
struct EncodedBall final : DensityField {
    static constexpr double radius = 20'000.0;
    DistanceEncoding encoding = DistanceEncoding::linear;
    Sample sample(std::int64_t x, std::int64_t y, std::int64_t z) const override {
        const double px = static_cast<double>(x) * lod0_cell_meters, py = static_cast<double>(y) * lod0_cell_meters,
                     pz = static_cast<double>(z) * lod0_cell_meters;
        return {encode_distance(std::sqrt(px * px + py * py + pz * pz) - radius, encoding), 1};
    }
    static bool may_have_surface(const ChunkKey& k) {
        const auto o = chunk_origin(k);
        const double e = static_cast<double>(chunk_units(k.level)) * lod0_cell_meters;
        double near2 = 0.0, far2 = 0.0;
        for (std::size_t a = 0; a < 3; ++a) {
            const double lo = static_cast<double>(o[a]) * lod0_cell_meters, hi = lo + e;
            const double n = lo > 0.0 ? lo : (hi < 0.0 ? -hi : 0.0);
            const double f = std::max(std::abs(lo), std::abs(hi));
            near2 += n * n;
            far2 += f * f;
        }
        return near2 <= radius * radius && far2 >= radius * radius;
    }
};

struct CoarseMeshes {
    std::vector<MeshedChunk> chunks;
    double mean_radial = 0.0;  ///< mean of normal . radial direction over vertices (1 for a sphere)
    std::array<int, 12> levels{};
};

/// The whole ball, three levels around 128-512 m cells, meshed with `encoding`.
CoarseMeshes mesh_coarse(DistanceEncoding encoding) {
    LodSettings s;
    s.focus = {0.0, 1.5 * EncodedBall::radius / lod0_cell_meters, 0.0};  // 10 km above the top
    s.lambda = 1.0;
    s.root_level = 11;  // 32 km roots
    s.root_min = {-1, -1, -1};
    s.root_max = {0, 0, 0};
    const auto leaves = select_chunks(s);
    EncodedBall field;
    field.encoding = encoding;
    auto mesher = make_transvoxel_mesher({.encoding = encoding});
    CoarseMeshes out;
    double radial = 0.0;
    std::size_t vertices = 0;
    ChunkSamples samples;
    for (const ChunkLeaf& leaf : leaves) {
        MeshedChunk m;
        m.leaf = leaf;
        m.mesh.key = leaf.key;
        if (EncodedBall::may_have_surface(leaf.key)) {
            sample_chunk(field, leaf.key, mesher->face_samples_needed(leaf.neighbors), samples);
            mesher->mesh(samples, leaf.neighbors, m.mesh);
        }
        if (!m.mesh.indices.empty()) {
            ++out.levels[std::min<std::size_t>(leaf.key.level, out.levels.size() - 1)];
        }
        for (std::size_t v = 0; v < m.mesh.fixed.size(); ++v) {
            std::array<double, 3> p{};
            for (std::size_t a = 0; a < 3; ++a) {
                p[a] = static_cast<double>(m.mesh.fixed[v][a]);
            }
            const double l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            radial += (p[0] * m.mesh.normal[v][0] + p[1] * m.mesh.normal[v][1] + p[2] * m.mesh.normal[v][2]) / l;
            ++vertices;
        }
        out.chunks.push_back(std::move(m));
    }
    out.mean_radial = radial / static_cast<double>(std::max<std::size_t>(vertices, 1));
    return out;
}

}  // namespace

TEST_CASE("[sp09] companded distances: linear within 16 m, monotonic, about 0.1 % steps beyond") {
    for (const DistanceEncoding e : {DistanceEncoding::linear, DistanceEncoding::companded}) {
        for (const double d : {0.0, 0.0004, -0.0004, 1.0, -3.25, 15.999, -16.0}) {
            CHECK(encode_distance(d, e) == encode_distance(d, DistanceEncoding::linear));
            CHECK(std::abs(decode_distance(encode_distance(d, e), e) - d) <= 0.5 / 1024.0);
        }
    }
    CHECK(encode_distance(1.0e6, DistanceEncoding::linear) == 32767);
    CHECK(encode_distance(-1.0e9, DistanceEncoding::companded) == -32767);
    std::int16_t previous = encode_distance(16.0, DistanceEncoding::companded);
    std::uint64_t h = 0;
    for (double d = 16.0; d < 1.0e7; d *= 1.01) {
        const std::int16_t c = encode_distance(d, DistanceEncoding::companded);
        CHECK(c >= previous);
        CHECK(encode_distance(-d, DistanceEncoding::companded) == -c);
        const double back = decode_distance(c, DistanceEncoding::companded);
        CHECK(std::abs(back - d) <= 0.0006 * d + 0.001);
        h = veach::hash(h, static_cast<std::uint64_t>(static_cast<std::uint16_t>(c)));
        previous = c;
    }
    // Canonical (dmath): committed from macOS arm64; Windows x64 CI must agree.
    CHECK(h == 0x4bddb71cd6a843c4ULL);
}

TEST_CASE("[sp09] coarse chunks: companded samples stay watertight and their normals follow the surface") {
    const CoarseMeshes companded = mesh_coarse(DistanceEncoding::companded);
    const CoarseMeshes linear = mesh_coarse(DistanceEncoding::linear);
    CHECK(companded.levels[8] + companded.levels[9] + companded.levels[10] > 0);
    const std::array<std::int64_t, 3> lo{-65536, -65536, -65536}, hi{65536, 65536, 65536};
    const SeamReport r = check_seams(companded.chunks, lo, hi);
    CHECK(r.triangles > 1000);
    CHECK(r.open_edges == 0);
    CHECK(r.boundary_edges == 0);  // the whole body: closed
    CHECK(r.nonmanifold_edges == 0);
    MESSAGE("mean normal . radial: companded ", companded.mean_radial, ", linear ", linear.mean_radial,
            "; meshed chunks L7 ", companded.levels[7], " L8 ", companded.levels[8], " L9 ", companded.levels[9],
            " L10 ", companded.levels[10]);
    CHECK(companded.mean_radial > 0.999);
    CHECK(companded.mean_radial > linear.mean_radial + 0.01);
    CHECK(meshes_hash(companded.chunks) == 0xdef38782f7377bf9ULL);  // committed from macOS arm64
}
