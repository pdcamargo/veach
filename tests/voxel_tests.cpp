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

TEST_CASE("[sp08] marching cubes + Transvoxel is watertight across LOD seams; plain marching cubes is not") {
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

TEST_CASE("[sp08] surface nets closes same-level seams; only chunks touching another level are open") {
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
