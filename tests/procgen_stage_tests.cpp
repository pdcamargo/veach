// engine/procgen's stage registry and runtime (docs/specs/procgen/spec.md). SP-05's real stages on it are tested in
// space_gen_tests.cpp ([procgen.AC9]).

#include <veach/core/jobs.hpp>
#include <veach/procgen/stage.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

using namespace veach;
namespace pg = veach::procgen;

namespace {

struct Root {
    std::uint64_t seed = 0;
};
struct Cell {
    std::uint64_t address = 0;
    std::uint64_t value = 0;
};
struct Summary {
    std::uint64_t total = 0;
};

std::uint64_t hash_root(const Root& r) {
    pg::RecordHasher h("test:root", 1);
    h.add(r.seed);
    return h.value();
}
std::uint64_t hash_cell(const Cell& c) {
    pg::RecordHasher h("test:cell", 1);
    h.add(c.address);
    h.add(c.value);
    return h.value();
}
std::uint64_t hash_summary(const Summary& s) {
    pg::RecordHasher h("test:summary", 1);
    h.add(s.total);
    return h.value();
}

/// A three-stage pipeline: a root at address 0, a cell per address reading the root, a summary per address reading
/// its cell. Counts how often each stage function runs.
struct Pipeline {
    pg::StageRegistry registry;
    pg::Stage<Root> root{};
    pg::Stage<Cell> cell{};
    pg::Stage<Summary> summary{};
    std::atomic<int> cell_runs{0};
    std::atomic<int> summary_runs{0};

    Pipeline() {
        root = *registry.add<Root>({.id = "test:root"}, &hash_root,
                                   [](const pg::StageContext& ctx) { return Root{ctx.universe_seed}; });
        cell = *registry.add<Cell>(
            {.id = "test:cell"}, &hash_cell,
            [this](const pg::StageContext& ctx, const Root& r) {
                ++cell_runs;
                return Cell{ctx.address, ctx.stream("value").next() ^ r.seed};
            },
            pg::from(root, pg::root_address));
        summary = *registry.add<Summary>(
            {.id = "test:summary", .stability = pg::Stability::pinned}, &hash_summary,
            [this](const pg::StageContext&, const Cell& c) -> Result<Summary> {
                ++summary_runs;
                if (c.address == 13) {
                    return make_error(ErrorCode::internal, "address 13 is unlucky", "use another address");
                }
                return Summary{c.value % 1000};
            },
            pg::from(cell));
    }
};

}  // namespace

TEST_CASE("[procgen.AC7] the registry keeps versions side by side and refuses what it cannot run") {
    pg::StageRegistry registry;
    const auto v1 = registry.add<Root>({.id = "test:root", .version = 1}, &hash_root,
                                       [](const pg::StageContext&) { return Root{1}; });
    const auto v2 = registry.add<Root>({.id = "test:root", .version = 2}, &hash_root,
                                       [](const pg::StageContext&) { return Root{2}; });
    REQUIRE(v1.has_value());
    REQUIRE(v2.has_value());
    CHECK(registry.versions("test:root") == std::vector<std::uint32_t>{1, 2});
    CHECK(registry.find<Root>("test:root", 2)->index == v2->index);
    CHECK_FALSE(registry.find<Cell>("test:root", 2).has_value());  // wrong output type
    CHECK(registry.info(*v1).stability == pg::Stability::frozen);

    const auto twice = registry.add<Root>({.id = "test:root", .version = 2}, &hash_root,
                                          [](const pg::StageContext&) { return Root{3}; });
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == ErrorCode::procgen_stage_invalid);
    CHECK(twice.error().hint.find("version") != std::string::npos);

    for (const char* bad : {"root", "Test:root", "test:", ":root", "test:ro ot"}) {
        CAPTURE(bad);
        const auto r = registry.add<Root>({.id = bad}, &hash_root, [](const pg::StageContext&) { return Root{}; });
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::procgen_stage_invalid);
    }
    const auto zero = registry.add<Root>({.id = "test:zero", .version = 0}, &hash_root,
                                         [](const pg::StageContext&) { return Root{}; });
    CHECK_FALSE(zero.has_value());

    // ADR 0043: data flows only downward. A visual stage may read a canonical one, never the other way round.
    const auto look = registry.add<Cell>(
        {.id = "test:look", .tier = pg::Tier::visual}, &hash_cell,
        [](const pg::StageContext&, const Root& r) { return Cell{0, r.seed}; }, pg::from(*v1));
    REQUIRE(look.has_value());
    const auto upward = registry.add<Summary>(
        {.id = "test:physics"}, &hash_summary, [](const pg::StageContext&, const Cell& c) { return Summary{c.value}; },
        pg::from(*look));
    REQUIRE_FALSE(upward.has_value());
    CHECK(upward.error().code == ErrorCode::procgen_stage_invalid);
    CHECK(upward.error().message.find("visual") != std::string::npos);
}

TEST_CASE(
    "[procgen.AC8] the runtime memoizes by stage, address and inputs, overrides feed downstream, failures "
    "keep nothing") {
    Pipeline p;
    pg::StageRuntime runtime(p.registry, {.universe_seed = 42, .manifest_fingerprint = 7});

    // The seed is ADR 0050's formula, and a stage reads its inputs at the addresses it declared.
    auto c5 = runtime.get(p.cell, 5);
    REQUIRE(c5.has_value());
    CHECK((*c5)->address == 5);
    const std::uint64_t seed = veach::hash(42, 5, veach::hash_id("test:cell"), 1);
    CHECK(pg::stage_seed(42, 5, "test:cell", 1) == seed);
    CHECK((*c5)->value == (HashStream::make(seed, "value", 0).next() ^ 42u));
    CHECK(*runtime.hash(p.cell, 5) == hash_cell(**c5));

    // Once per key.
    REQUIRE(runtime.get(p.summary, 5).has_value());
    REQUIRE(runtime.get(p.summary, 5).has_value());
    REQUIRE(runtime.get(p.summary, 6).has_value());
    CHECK(p.cell_runs == 2);
    CHECK(p.summary_runs == 2);
    const pg::CacheStats before = runtime.stats();
    CHECK(before.entries == 5);  // root, two cells, two summaries
    CHECK(before.hits >= 2);

    // An authored cell at 5 changes the summary at 5 only, and its hash.
    const std::uint64_t old_summary = *runtime.hash(p.summary, 5);
    runtime.override(p.cell, 5, Cell{5, 123456});
    CHECK((*runtime.get(p.cell, 5))->value == 123456);
    CHECK((*runtime.get(p.summary, 5))->total == 456);
    CHECK(*runtime.hash(p.summary, 5) != old_summary);
    REQUIRE(runtime.get(p.summary, 6).has_value());
    CHECK(p.cell_runs == 2);
    CHECK(p.summary_runs == 3);
    runtime.clear_override(p.cell, 5);
    CHECK(*runtime.hash(p.summary, 5) == old_summary);  // still cached under the old input hash
    CHECK(p.summary_runs == 3);

    // A failing stage names itself and the address, keeps the cause's hint and caches nothing.
    auto bad = runtime.get(p.summary, 13);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::procgen_stage_failed);
    CHECK(bad.error().message.find("'test:summary'@1") != std::string::npos);
    CHECK(bad.error().message.find("0x000000000000000d") != std::string::npos);
    CHECK(bad.error().message.find("unlucky") != std::string::npos);
    CHECK(bad.error().hint == "use another address");
    CHECK_FALSE(runtime.get(p.summary, 13).has_value());
    CHECK(p.summary_runs == 5);  // ran again: failures are not cached

    // Another fingerprint is another universe: nothing is shared, the records are the same.
    pg::StageRuntime other(p.registry, {.universe_seed = 42, .manifest_fingerprint = 8});
    CHECK(*other.hash(p.cell, 6) == *runtime.hash(p.cell, 6));

    // An LRU capacity bounds the cache.
    pg::StageRuntime small(p.registry, {.universe_seed = 42, .cache_capacity = 4});
    for (pg::Address a = 100; a < 110; ++a) {
        REQUIRE(small.get(p.summary, a).has_value());
    }
    CHECK(small.stats().entries == 4);
    CHECK(small.stats().evictions > 0);
}

TEST_CASE("[procgen.AC8] the runtime is safe from many jobs, and results don't depend on the thread count") {
    std::vector<std::uint64_t> expected(2000);
    {
        Pipeline p;
        pg::StageRuntime runtime(p.registry, {.universe_seed = 9});
        for (std::size_t i = 0; i < expected.size(); ++i) {
            expected[i] = *runtime.hash(p.summary, 1000 + i);
        }
    }
    for (const std::uint32_t threads : {1u, 4u}) {
        Pipeline p;
        pg::StageRuntime runtime(p.registry, {.universe_seed = 9, .cache_capacity = 64});
        JobSystem jobs(threads);
        std::vector<std::uint64_t> got(expected.size());
        jobs.parallel_for("stages", got.size(), 16, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = end; i-- > begin;) {  // backward within a chunk
                got[i] = *runtime.hash(p.summary, 1000 + i);
            }
        });
        CHECK(got == expected);
    }
}
