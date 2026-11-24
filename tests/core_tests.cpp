#include "support/test_support.hpp"

#include <doctest/doctest.h>

#include <set>

using namespace veach;

TEST_CASE("error codes are unique and every one is documented") {
    auto docs = fs::read_file(test::source_dir() / "docs/reference/errors.md");
    REQUIRE(docs.has_value());
    std::set<std::string_view> seen;
    for (const ErrorCode code : all_error_codes()) {
        const std::string_view s = slug(code);
        CHECK_MESSAGE(seen.insert(s).second, "duplicate slug ", s);
        CHECK_MESSAGE(docs->find(std::format("## {}", s)) != std::string::npos,
                      "docs/reference/errors.md has no section for ", s);
    }
}

TEST_CASE("Error::to_string includes hint and code") {
    const Error e{ErrorCode::script_unknown_component, "unknown component 'Tranform'", "Did you mean 'Transform'?"};
    CHECK(e.to_string() == "unknown component 'Tranform' — Did you mean 'Transform'? [script.unknown_component]");
}

TEST_CASE("did_you_mean suggests close names only") {
    const std::vector<std::string> names{"Transform", "Velocity", "Health"};
    CHECK(did_you_mean("Tranform", names) == "Did you mean 'Transform'?");
    CHECK(did_you_mean("velocity", names) == "Did you mean 'Velocity'?");
    CHECK(did_you_mean("Banana", names).empty());
}

TEST_CASE("AssetId round-trips through its string form") {
    const AssetId id = AssetId::generate();
    CHECK(id.is_valid());
    const auto parsed = AssetId::parse(id.to_string());
    REQUIRE(parsed.has_value());
    CHECK(*parsed == id);
    CHECK_FALSE(AssetId::parse("not-a-uuid").has_value());
    CHECK(id.to_string()[14] == '4');  // version 4
}

TEST_CASE("Rng is deterministic for a seed") {
    Rng a(42);
    Rng b(42);
    Rng c(43);
    for (int i = 0; i < 100; ++i) {
        const std::uint32_t x = a.next_u32();
        CHECK(x == b.next_u32());
        static_cast<void>(c.next_u32());
    }
    CHECK(Rng(42).next_u32() != Rng(43).next_u32());
    const float f = a.next_float();
    CHECK(f >= 0.0f);
    CHECK(f < 1.0f);
}

TEST_CASE("FixedTimestep converts frame time into whole 60 Hz steps") {
    FixedTimestep fixed;
    CHECK(fixed.advance(1.0 / 120.0) == 0);
    CHECK(fixed.advance(1.0 / 120.0) == 1);
    CHECK(fixed.advance(3.0 / 60.0) == 3);
    CHECK(fixed.advance(10.0) == 8);  // clamped, backlog dropped
    CHECK(fixed.advance(0.0) == 0);
}

TEST_CASE("reverse-Z projection maps near to 1 and far to 0") {
    const math::mat4 proj = math::perspective_reverse_z(math::radians(60.0f), 16.0f / 9.0f, 0.1f);
    const auto depth = [&](float view_z) {
        const math::vec4 clip = proj * math::vec4(0.0f, 0.0f, view_z, 1.0f);
        return clip.z / clip.w;
    };
    CHECK(depth(-0.1f) == doctest::Approx(1.0f));
    CHECK(depth(-1000.0f) == doctest::Approx(0.0001f).epsilon(0.01));
    CHECK(depth(-1.0f) > depth(-10.0f));  // closer is larger
    CHECK(math::world_forward.z == -1.0f);
    CHECK(math::world_up.y == 1.0f);
}

TEST_CASE("JSON log lines are valid single-line JSON") {
    const log::Record record{log::Level::error, "script", "bad \"quote\"\nnext", "script.runtime_error", 1.5};
    const std::string line = log::format_record(record, log::Format::json);
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line ==
          R"({"t":1.500000,"level":"error","cat":"script","msg":"bad \"quote\"\nnext","code":"script.runtime_error"})");
}
