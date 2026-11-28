// Spike SP-05 (VCH-081), throwaway: veach::hash test vectors (ADR 0044), system addresses (ADR 0050), and S1/B1
// stage hashes (ADR 0051): committed values the macOS gate and Windows CI both check, equal across thread counts and
// generation order, and equal at every scale. docs/research/spikes/sp-05-system-statistics.md. Since VCH-112 the
// stages run on engine/procgen's stage runtime, which must give the same hashes (procgen.AC9).

#include <doctest/doctest.h>
#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/procgen/stage.hpp>
#include <veach/space/gen/address.hpp>
#include <veach/space/gen/realize.hpp>
#include <veach/space/gen/stages.hpp>
#include <veach/space/gen/system.hpp>

#include <cstdint>
#include <format>
#include <memory>
#include <string>
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

/// SP-05's stages on the engine's stage runtime (VCH-112), for the sample universe.
struct Runtime {
    veach::procgen::StageRegistry registry;
    sg::Stages stages{};
    std::unique_ptr<veach::procgen::StageRuntime> runtime;

    explicit Runtime(std::size_t capacity = 0) {
        auto s = sg::register_stages(registry, sample_universe);
        REQUIRE_MESSAGE(s.has_value(), (s ? std::string() : s.error().message));
        stages = *s;
        runtime = std::make_unique<veach::procgen::StageRuntime>(
            registry,
            veach::procgen::RuntimeOptions{.universe_seed = sample_universe.seed, .cache_capacity = capacity});
    }

    StageHashes hashes(std::uint64_t i) const {
        const std::uint64_t a = sample_address(i).value;
        return {*runtime->hash(stages.system, a), *runtime->hash(stages.body_physics, a)};
    }
};

}  // namespace

TEST_CASE("[sp05][procgen.AC9] S1 and B1 stage hashes of a fixed sample match the committed ones, on the runtime") {
    Runtime rt;
    std::vector<StageHashes> direct(sample_size), on_runtime(sample_size);
    sg::Violations v;
    for (std::uint64_t i = 0; i < sample_size; ++i) {
        direct[i] = generate(i);
        on_runtime[i] = rt.hashes(i);
        const auto system = rt.runtime->get(rt.stages.system, sample_address(i).value);
        const auto physics = rt.runtime->get(rt.stages.body_physics, sample_address(i).value);
        REQUIRE(system.has_value());
        REQUIRE(physics.has_value());
        v += sg::validate(**system, **physics);
    }
    const StageHashes c = combine(direct);
    const StageHashes r = combine(on_runtime);
    INFO(std::format("S1 0x{:016x}, B1 0x{:016x}", c.s1, c.b1));
    // Regenerate only on purpose: a different value is a stage version bump (system_stage_version,
    // body_physics_stage_version), or a dmath or veach::hash change.
    CHECK(c.s1 == 0xc28a0456eedc0301ULL);
    CHECK(c.b1 == 0xf2f6a4c18dbe873dULL);
    CHECK(r.s1 == c.s1);
    CHECK(r.b1 == c.b1);
    CHECK(v.total() == 0);
    // The runtime's seed is the one the stages always used (ADR 0050).
    const sg::StageContext ctx{&sample_universe, sample_address(5)};
    CHECK(veach::procgen::StageContext{sample_universe.seed, sample_address(5).value, sg::system_stage_id,
                                       sg::system_stage_version}
              .seed() == sg::stage_seed(ctx, sg::system_stage_id, sg::system_stage_version));
}

TEST_CASE("[sp05][procgen.AC9] stage hashes don't depend on thread count or generation order") {
    std::vector<StageHashes> one(sample_size);
    for (std::uint64_t i = 0; i < sample_size; ++i) {
        one[i] = generate(i);
    }
    for (const std::uint32_t threads : {1u, 4u}) {
        Runtime rt(256);  // a small cache: evictions mid-run must not change anything
        veach::JobSystem jobs(threads);
        std::vector<StageHashes> got(sample_size);
        jobs.parallel_for("sp05 stages", sample_size, 25, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = end; i-- > begin;) {  // backward within each chunk
                got[i] = rt.hashes(i);
            }
        });
        const StageHashes a = combine(one), b = combine(got);
        CHECK(a.s1 == b.s1);
        CHECK(a.b1 == b.b1);
        CHECK(rt.runtime->stats().evictions > 0);
    }
}

TEST_CASE("[sp05] records are equal at s = 1, 0.1 and 0.01, only realized geometry scales") {
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
