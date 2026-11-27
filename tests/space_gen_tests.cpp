// Spike SP-05 (VCH-081), throwaway: veach::hash test vectors (ADR 0044), system addresses (ADR 0050), and S1/B1
// stage hashes (ADR 0051): committed values the macOS gate and Windows CI both check, equal across thread counts and
// generation order, and equal at every scale. docs/research/spikes/sp-05-system-statistics.md.

#include <doctest/doctest.h>
#include <veach/core/hash.hpp>
#include <veach/space/gen/address.hpp>
#include <veach/space/gen/realize.hpp>
#include <veach/space/gen/system.hpp>

#include <cstdint>
#include <format>
#include <thread>
#include <vector>

namespace sg = space::gen;

// Reference vectors that pin the building blocks to published algorithms.
static_assert(veach::hash_id("") == 0xCBF29CE484222325ULL);   // FNV-1a 64 offset basis
static_assert(veach::hash_id("a") == 0xAF63DC4C8601EC8CULL);  // FNV-1a 64 of "a"
// SplitMix64's first output for seed 1234567 (Vigna's splitmix64.c): mix64(state + golden).
static_assert(veach::mix64(1234567ULL + veach::hash_golden) == 6457827717110365317ULL);
static_assert(veach::mul_high64(1ULL << 63, 4) == 2);
static_assert(veach::mul_high64(~0ULL, ~0ULL) == 0xFFFFFFFFFFFFFFFEULL);
static_assert(veach::unit_double(~0ULL) == 1.0 - 0x1p-53);
static_assert(veach::unit_double(0) == 0.0);
static_assert(veach::range(0, 0, 9) == 0 && veach::range(~0ULL, 0, 9) == 9 && veach::range(12345, 5, 5) == 5);

TEST_CASE("[sp05] veach::hash gives the committed test vectors") {
    // Regenerate only on purpose: a different value is a determinism contract change (ADR 0044, 0055).
    CHECK(veach::hash(0, 0) == 0x294cf66702b428c5ULL);
    CHECK(veach::hash(1, 2, 3, 4) == 0xd881bf874e1977b6ULL);
    CHECK(veach::hash(42, veach::hash_id("orbits"), 0x3f2a91c07e5b0000ULL, 7) == 0x16aa83b1f5698304ULL);
    CHECK(veach::hash(1, 2, 3, 4, 5, 6, 7) == 0xc4ab8f2e7941360dULL);
    CHECK(veach::hash_id("space:system") == 0x4fb6d5a2eba0be84ULL);
    CHECK(veach::hash(1, 2) != veach::hash(2, 1));
    CHECK(veach::hash(1, 2, 3) != veach::hash(1, 3, 2));
    veach::HashStream s = veach::HashStream::make(42, "orbits", 7);
    CHECK(s.key == veach::hash(42, veach::hash_id("orbits"), 7));
    CHECK(s.next() == veach::hash(s.key, 0));
    CHECK(s.next() == veach::hash(s.key, 1));
    CHECK(s.counter == 2);
}

TEST_CASE("[sp05] system addresses encode, decode and print at every layer") {
    for (std::uint32_t layer = 0; layer < sg::address_layers; ++layer) {
        CAPTURE(layer);
        CHECK(3 + 2 * sg::cell_bits_xz(layer) + sg::cell_bits_y(layer) + sg::system_bits(layer) + sg::body_bits == 64);
        const sg::SystemCoords max{layer, (1u << sg::cell_bits_xz(layer)) - 1u, (1u << sg::cell_bits_y(layer)) - 1u,
                                   (1u << sg::cell_bits_xz(layer)) - 1u,
                                   (std::uint64_t{1} << sg::system_bits(layer)) - 1u};
        const auto a = sg::SystemAddress::encode(max);
        REQUIRE(a.has_value());
        CHECK(a->decode() == max);
        CHECK(sg::parse_system_address(sg::to_string(*a)) == a);
        const sg::BodyAddress b = a->body(255);
        CHECK(b.index() == 255);
        CHECK(b.system() == *a);
        sg::SystemCoords over = max;
        ++over.system;
        CHECK_FALSE(sg::SystemAddress::encode(over).has_value());
        over = max;
        ++over.y;
        CHECK_FALSE(sg::SystemAddress::encode(over).has_value());
    }
    // The layout, high to low: layer | x | z | y | system | body.
    CHECK(sg::SystemAddress::encode({0, 1, 0, 0, 0})->value == 1ULL << (9 + 13 + 11 + 14));
    CHECK(sg::SystemAddress::encode({7, 0, 0, 0, 0})->value == 7ULL << 61);
    CHECK(sg::SystemAddress::encode({0, 0, 0, 0, 1})->value == 1ULL << 9);
    CHECK(sg::to_string(sg::SystemAddress{0x3f2a91c07e5b0000ULL}.body(4)) == "S:3f2a91c07e5b0000/B:4");
    CHECK_FALSE(sg::parse_system_address("S:3f2a91c07e5b0004").has_value());  // body bits set
    CHECK_FALSE(sg::SystemAddress::encode({8, 0, 0, 0, 0}).has_value());
}

namespace {

/// The bench's address spread (tests/bench/sp05_system_stats.cpp): all 8 layers, 64 systems per cell.
sg::SystemAddress sample_address(std::uint64_t i) {
    sg::SystemCoords c;
    c.layer = static_cast<std::uint32_t>(i % 8u);
    const std::uint64_t j = i / 8u;
    c.system = j % 64u;
    const std::uint64_t cell = j / 64u;
    c.x = (1u << (sg::cell_bits_xz(c.layer) - 1u)) + static_cast<std::uint32_t>(cell % 16u);
    c.z = (1u << (sg::cell_bits_xz(c.layer) - 1u)) + static_cast<std::uint32_t>((cell / 16u) % 16u);
    c.y = (1u << (sg::cell_bits_y(c.layer) - 1u)) + static_cast<std::uint32_t>((cell / 256u) % 8u);
    return *sg::SystemAddress::encode(c);
}

constexpr std::uint64_t sample_size = 2000;
const sg::UniverseRecord sample_universe{42, 0.5};

struct StageHashes {
    std::uint64_t s1 = 0, b1 = 0;
};

StageHashes generate(std::uint64_t i, const sg::ScaleFactors* realize_at = nullptr) {
    const sg::StageContext ctx{&sample_universe, sample_address(i)};
    const sg::SystemRecord r = sg::generate_system(ctx);
    const std::vector<sg::BodyPhysics> p = sg::derive_all_body_physics(ctx, r);
    if (realize_at != nullptr) {
        (void)sg::realize(r, p, *realize_at);
    }
    return {sg::stage_hash(r), sg::stage_hash(p)};
}

StageHashes combine(const std::vector<StageHashes>& h) {  // in index order
    StageHashes out{veach::hash_id("s1"), veach::hash_id("b1")};
    for (const StageHashes& x : h) {
        out.s1 = veach::hash(out.s1, x.s1);
        out.b1 = veach::hash(out.b1, x.b1);
    }
    return out;
}

}  // namespace

TEST_CASE("[sp05] S1 and B1 stage hashes of a fixed sample match the committed ones") {
    std::vector<StageHashes> h(sample_size);
    sg::Violations v;
    for (std::uint64_t i = 0; i < sample_size; ++i) {
        h[i] = generate(i);
        const sg::StageContext ctx{&sample_universe, sample_address(i)};
        const sg::SystemRecord r = sg::generate_system(ctx);
        v += sg::validate(r, sg::derive_all_body_physics(ctx, r));
    }
    const StageHashes c = combine(h);
    INFO(std::format("S1 0x{:016x}, B1 0x{:016x}", c.s1, c.b1));
    // Regenerate only on purpose: a different value is a stage version bump (system_stage_version,
    // body_physics_stage_version), or a dmath or veach::hash change.
    CHECK(c.s1 == 0xc28a0456eedc0301ULL);
    CHECK(c.b1 == 0xf2f6a4c18dbe873dULL);
    CHECK(v.total() == 0);
}

TEST_CASE("[sp05] stage hashes don't depend on thread count or generation order") {
    std::vector<StageHashes> one(sample_size), four(sample_size);
    for (std::uint64_t i = 0; i < sample_size; ++i) {
        one[i] = generate(i);
    }
    std::vector<std::thread> pool;
    for (std::uint64_t t = 0; t < 4; ++t) {
        pool.emplace_back([&four, t] {
            for (std::uint64_t i = sample_size; i-- > 0;) {  // backward, interleaved
                if (i % 4 == t) {
                    four[i] = generate(i);
                }
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    const StageHashes a = combine(one), b = combine(four);
    CHECK(a.s1 == b.s1);
    CHECK(a.b1 == b.b1);
}

TEST_CASE("[sp05] records are equal at s = 1, 0.1 and 0.01; only realized geometry scales") {
    // The stage API has no scale input (StageContext has none); s reaches realize() only.
    const sg::ScaleFactors scales[] = {{1.0, 1.0}, {0.1, 0.1}, {0.01, 0.01}};
    std::vector<StageHashes> at[3];
    for (int k = 0; k < 3; ++k) {
        for (std::uint64_t i = 0; i < 200; ++i) {
            at[k].push_back(generate(i, &scales[k]));
        }
    }
    CHECK(combine(at[0]).s1 == combine(at[1]).s1);
    CHECK(combine(at[1]).s1 == combine(at[2]).s1);
    CHECK(combine(at[0]).b1 == combine(at[2]).b1);

    const sg::StageContext ctx{&sample_universe, sample_address(3)};
    const sg::SystemRecord r = sg::generate_system(ctx);
    REQUIRE(!r.bodies.empty());
    const std::vector<sg::BodyPhysics> p = sg::derive_all_body_physics(ctx, r);
    const sg::RealizedSystem big = sg::realize(r, p, scales[0]), small = sg::realize(r, p, scales[2]);
    CHECK(sg::realized_hash(big) != sg::realized_hash(small));
    CHECK(small.bodies[0].radius_m == doctest::Approx(0.01 * big.bodies[0].radius_m));
    // Surface gravity stays physical: GM / R² is the same at both scales.
    const double g_big = big.bodies[0].gm_m3_s2 / (big.bodies[0].radius_m * big.bodies[0].radius_m);
    const double g_small = small.bodies[0].gm_m3_s2 / (small.bodies[0].radius_m * small.bodies[0].radius_m);
    CHECK(g_small == doctest::Approx(g_big));
    const sg::RealizedViolations rv = sg::validate_realized(r, small);
    CHECK(rv.moon_hill + rv.moon_roche == 0);
}
