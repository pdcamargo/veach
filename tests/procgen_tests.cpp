// engine/procgen (docs/specs/procgen/spec.md): field graphs and their canonical CPU interpreter. The committed values
// come from spike SP-06 (docs/research/spikes/sp-06-field-graph.md) and must not change: the macOS gate and Windows
// CI both check them (ADR 0043). Stage runtime tests are in procgen_stage_tests.cpp.

#include "support/test_support.hpp"

#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/platform/fs.hpp>
#include <veach/procgen/field_graph.hpp>
#include <veach/procgen/sandbox.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

using namespace veach;
namespace pg = veach::procgen;

namespace {

std::string terrain_json() {
    auto text = fs::read_file(test::source_dir() / "tests/procgen/terrain.field.json");
    REQUIRE_MESSAGE(text.has_value(), (text ? std::string() : text.error().message));
    return *text;
}

pg::FieldGraph parse(std::string_view json) {
    auto graph = pg::parse_field_graph(json, "test graph");
    REQUIRE_MESSAGE(graph.has_value(), (graph ? std::string() : graph.error().message));
    return *graph;
}

pg::FieldProgram program_of(std::string_view json, const pg::BodyConstants& body) {
    auto program = pg::compile(parse(json), body);
    REQUIRE_MESSAGE(program.has_value(), (program ? std::string() : program.error().message));
    return std::move(*program);
}

pg::FieldProgram terrain(std::uint64_t seed = 42) {
    return program_of(terrain_json(), {.seed = seed, .radius_planet_m = 6.0e6});
}

/// The ops of a program as (opcode, payload) pairs.
std::vector<std::pair<pg::Op, std::vector<std::uint32_t>>> ops(const pg::FieldProgram& p) {
    std::vector<std::pair<pg::Op, std::vector<std::uint32_t>>> out;
    for (std::size_t pc = 0; pc < p.words.size(); pc += pg::op_header_words + p.words[pc + 5]) {
        const auto* payload = p.words.data() + pc + pg::op_header_words;
        out.emplace_back(static_cast<pg::Op>(p.words[pc]), std::vector(payload, payload + p.words[pc + 5]));
    }
    return out;
}

Error compile_error(std::string_view json) {
    auto graph = pg::parse_field_graph(json);
    if (graph) {
        auto program = pg::compile(*graph, {});
        REQUIRE_FALSE(program.has_value());
        return program.error();
    }
    return graph.error();
}

std::uint64_t hash_floats(std::span<const float> values) {
    std::uint64_t h = 0;
    for (const float v : values) {
        h = mix64(h ^ std::bit_cast<std::uint32_t>(v));
    }
    return h;
}

bool bit_equal(std::span<const float> a, std::span<const float> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

/// A cave-scale graph on realized meters: what terrain density looks like at collider resolution.
constexpr std::string_view caves_json = R"({
  "veach_graph": 1, "id": "test:caves", "inputs": { "p": "p_local" },
  "nodes": [
    { "id": "tunnels", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "m": 24 }, "octaves": 4 } },
    { "id": "rock", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "m": 3 }, "octaves": 3, "gain": 0.4 } },
    { "id": "walls", "type": "veach:ridge@1", "inputs": { "x": "tunnels" }, "params": { "offset": 0.6 } },
    { "id": "density", "type": "veach:add@1", "inputs": { "a": "walls", "b": "rock" } }
  ],
  "outputs": { "density": "density" }
})";

/// 6,000 km from the center along +x, in LOD0 units of 0.5 m: a chunk on the surface of an Earth-sized body.
constexpr std::int64_t surface_x = 12'000'000;
const pg::Lattice lod0{.spacing_m = 0.5, .origin_m = {0.0, 0.0, 0.0}};

std::vector<float> eval_box(const pg::FieldProgram& p, const pg::LatticeBox& box, pg::EvalOptions o = {}) {
    std::vector<float> out(p.outputs.size() * box.size());
    pg::evaluate(p, lod0, box, out, o);
    return out;
}

}  // namespace

TEST_CASE("[procgen.AC2] field graph errors name the node and the fix") {
    const std::string head = R"({"veach_graph": 1, "id": "t", "inputs": {"p": "p_planet"}, "outputs": {"h": "n"}, )";
    const Error bare = compile_error(head + R"("nodes": [{"id": "n", "type": "veach:fbm@1", "inputs": {"p": "p"},
                            "params": {"wavelength": 1000, "octaves": 2}}]})");
    CHECK(bare.code == ErrorCode::procgen_graph_invalid);
    CHECK(bare.message.find("'n'") != std::string::npos);
    CHECK(bare.hint.find("planet_m") != std::string::npos);

    const Error version =
        compile_error(head + R"("nodes": [{"id": "n", "type": "veach:fbm@2", "inputs": {"p": "p"}, "params": {}}]})");
    CHECK(version.code == ErrorCode::procgen_graph_invalid);
    CHECK(version.hint.find("veach:fbm@1") != std::string::npos);

    const Error param = compile_error(head + R"("nodes": [{"id": "n", "type": "veach:fbm@1", "inputs": {"p": "p"},
                            "params": {"wavelength": {"planet_m": 1000}, "octaves": 2, "persistence": 0.5}}]})");
    CHECK(param.code == ErrorCode::data_unknown_field);
    CHECK(param.hint.find("gain") != std::string::npos);

    const Error cycle =
        compile_error(head + R"("nodes": [{"id": "n", "type": "veach:add@1", "inputs": {"a": "m", "b": "m"}},
                                          {"id": "m", "type": "veach:mul@1", "inputs": {"a": "n", "b": "n"}}]})");
    CHECK(cycle.code == ErrorCode::procgen_graph_invalid);
    CHECK(cycle.message.find("cycle") != std::string::npos);

    const Error position =
        compile_error(head + R"("nodes": [{"id": "n", "type": "veach:ridge@1", "inputs": {"x": "p"}}]})");
    CHECK(position.code == ErrorCode::procgen_graph_invalid);
    CHECK(position.hint.find("node's output") != std::string::npos);

    // procgen.AC5's unit check: a meter length on a planet position.
    const Error unit = compile_error(head + R"("nodes": [{"id": "n", "type": "veach:fbm@1", "inputs": {"p": "p"},
                            "params": {"wavelength": {"m": 3}, "octaves": 2}}]})");
    CHECK(unit.code == ErrorCode::procgen_graph_invalid);
    CHECK(unit.message.find("p_planet") != std::string::npos);
    CHECK(unit.hint.find("p_local") != std::string::npos);

    CHECK(compile_error("{not json").code == ErrorCode::data_parse_failed);
    CHECK(compile_error(R"({"veach_graph": 2})").hint.find("format 1") != std::string::npos);
}

TEST_CASE("[procgen.AC3] the terrain graph compiles to one op list with folded constants and hashed seeds") {
    const pg::FieldProgram p = terrain();
    const auto list = ops(p);
    REQUIRE(list.size() == 8);  // every node is reachable from the output
    CHECK(p.noise_octaves == 11);
    CHECK(p.registers <= 4);
    REQUIRE(p.outputs.size() == 1);
    CHECK(p.outputs[0].name == "height");
    CHECK(list[0].first == pg::Op::fbm);
    // continents: 6 octaves; wavelength 3,000 km on a 6,000 km planet folds to frequency 2 on the unit sphere.
    CHECK(list[0].second[0] == 6);
    CHECK(std::bit_cast<float>(list[0].second[1]) == 2.0f);
    // The seeds tests/sp06/shaders/sp06_field_handwritten.slang bakes in: veach::hash(hash(42, hash_id(node)), octave),
    // folded to 32 bits (SP-06's committed values).
    const std::array<std::uint32_t, 6> continents{0x0D1C6AB3u, 0x2CA4B71Cu, 0x95510E19u,
                                                  0x376320F1u, 0x9BE8AC0Eu, 0x8413A8EDu};
    CHECK(std::equal(continents.begin(), continents.end(), list[0].second.begin() + 5));
    // After the seeds, the lattice frequencies in double: 1 / 3,000 km, doubling per octave.
    std::uint64_t f0 = std::uint64_t{list[0].second[11]} | (std::uint64_t{list[0].second[12]} << 32);
    CHECK(std::bit_cast<double>(f0) == 1.0 / 3.0e6);
    CHECK(list[0].second.size() == 5 + 6 + 2 * 6);
    const auto mountains =
        std::find_if(list.begin() + 1, list.end(), [](const auto& op) { return op.first == pg::Op::fbm; });
    REQUIRE(mountains != list.end());
    CHECK(std::bit_cast<float>(mountains->second[1]) == 10.0f);
    CHECK(mountains->second[5] == 0x561C8B05u);
    CHECK(mountains->second[9] == 0xC3058AD7u);
    // Another body seed gives other octave seeds and nothing else.
    const auto other = ops(terrain(43));
    CHECK(other[0].second[5] != list[0].second[5]);
    CHECK(std::equal(other[0].second.begin(), other[0].second.begin() + 5, list[0].second.begin()));
    CHECK(std::equal(other[0].second.begin() + 11, other[0].second.end(), list[0].second.begin() + 11));
    // The graph hash is the cache key: same text, same hash; the program carries it.
    CHECK(parse(terrain_json()).hash() == p.graph_hash);
    CHECK(parse(caves_json).hash() != p.graph_hash);
}

TEST_CASE("[procgen.AC4] on cube faces, batches equal single samples and the SP-06 hash is unchanged") {
    const pg::FieldProgram p = terrain();
    pg::FaceRegion region;
    region.resolution = 128;
    std::vector<float> batch(region.size());
    std::vector<float> scalar(region.size());
    std::vector<float> odd(region.size());
    pg::evaluate(p, region, batch);
    pg::evaluate(p, region, scalar, {.batch = 1});
    pg::evaluate(p, region, odd, {.batch = 77});  // batches that straddle rows
    CHECK(bit_equal(batch, scalar));
    CHECK(bit_equal(batch, odd));
    const auto [lo, hi] = std::minmax_element(batch.begin(), batch.end());
    CHECK(*lo >= 0.0f);
    CHECK(*hi <= 1.0f);
    CHECK(*hi - *lo > 0.5f);  // not flat
    // ADR 0043: the same bits on macOS arm64 (Apple Clang) and Windows x64 (MSVC), Debug and Release. SP-06's value.
    CHECK(hash_floats(batch) == 0xFB7136EA481F1F8Dull);

    // The same directions as a list give the same values (FaceRegion's projection, written out).
    std::vector<std::array<float, 3>> directions;
    const float step = 2.0f / 128.0f;
    for (std::uint32_t j = 0; j < 128; ++j) {
        for (std::uint32_t i = 0; i < 128; ++i) {
            const float u = (static_cast<float>(i) + 0.5f) * step - 1.0f;
            const float v = 1.0f - (static_cast<float>(j) + 0.5f) * step;
            const std::array<float, 3> c{u, v, 1.0f};  // center (0, 0, 1), right +x, up +y
            const float len = std::sqrt(std::fma(c[0], c[0], std::fma(c[1], c[1], c[2] * c[2])));
            directions.push_back({c[0] / len, c[1] / len, c[2] / len});
        }
    }
    std::vector<float> listed(directions.size());
    pg::evaluate(p, directions, listed, {.batch = 31});
    CHECK(bit_equal(listed, batch));
}

TEST_CASE("[procgen.AC4] without hardware FMA, the double-precision path gives fma's bits") {
    // field_eval.cpp's fused() rounds a * b + c through double when there is no float FMA (MSVC x64). Check that
    // claim here, where both exist, on the operands it gets (SP-06's test).
    HashStream rng = HashStream::make(7, "sp06.fused", 0);
    auto via_double = [](float a, float b, float c) {
        return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) + static_cast<double>(c));
    };
    int mismatches = 0;
    for (int i = 0; i < 200000; ++i) {
        // The lattice fraction: p on the unit sphere, frequencies up to 2^12, cell = floor(p * f).
        const auto p = static_cast<float>(rng.uniform(-1.0, 1.0));
        const auto f = static_cast<float>(rng.uniform(0.5, 4096.0));
        const float cell = std::floor(p * f);
        mismatches += std::fma(p, f, -cell) != via_double(p, f, -cell);
        // The fade polynomial, t in [0, 1).
        const auto t = static_cast<float>(rng.next_double());
        const float inner = std::fma(t, 6.0f, -15.0f);
        mismatches += inner != via_double(t, 6.0f, -15.0f);
        mismatches += std::fma(t, inner, 10.0f) != via_double(t, inner, 10.0f);
    }
    CHECK(mismatches == 0);
}

TEST_CASE("[procgen.AC5] a lattice sample is a function of its point alone: lists, boxes, steps, batches, threads") {
    const pg::FieldProgram caves = program_of(caves_json, {.seed = 7});
    const pg::FieldProgram hills = program_of(terrain_json(), {.seed = 42, .planet_m_per_m = 1000.0});  // s_p = 0.001
    for (const pg::FieldProgram* p : {&caves, &hills}) {
        // A level-0 chunk's corners on the surface, and the same region at levels 1 and 2.
        const pg::LatticeBox level0{.first = {surface_x, -16, 40}, .step = 1, .count = {33, 33, 33}};
        const pg::LatticeBox level1{.first = {surface_x, -16, 40}, .step = 2, .count = {17, 17, 17}};
        const pg::LatticeBox level2{.first = {surface_x - 8, -24, 32}, .step = 4, .count = {11, 11, 11}};
        const std::vector<float> a = eval_box(*p, level0);
        const std::vector<float> b = eval_box(*p, level1);
        const std::vector<float> c = eval_box(*p, level2);
        int shared = 0;
        int mismatches = 0;
        for (std::size_t i = 0; i < level0.size(); ++i) {
            const pg::LatticePoint q = level0.point(i);
            const std::int64_t x = q[0] - level0.first[0], y = q[1] - level0.first[1], z = q[2] - level0.first[2];
            if (x % 2 == 0 && y % 2 == 0 && z % 2 == 0) {
                const std::size_t j = static_cast<std::size_t>(x / 2 + 17 * (y / 2 + 17 * (z / 2)));
                REQUIRE(level1.point(j) == q);
                mismatches += std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(b[j]);
                ++shared;
            }
            if (x % 4 == 0 && y % 4 == 0 && z % 4 == 0) {
                const std::size_t j = static_cast<std::size_t>((x + 8) / 4 + 11 * ((y + 8) / 4 + 11 * ((z + 8) / 4)));
                REQUIRE(level2.point(j) == q);
                mismatches += std::bit_cast<std::uint32_t>(a[i]) != std::bit_cast<std::uint32_t>(c[j]);
                ++shared;
            }
        }
        CHECK(shared == 17 * 17 * 17 + 9 * 9 * 9);
        CHECK(mismatches == 0);

        // A shuffled point list, in batches of 256, 1 and 13.
        std::vector<pg::LatticePoint> points;
        HashStream pick = HashStream::make(3, "procgen.points", 0);
        std::vector<std::size_t> index;
        for (int k = 0; k < 1000; ++k) {
            index.push_back(static_cast<std::size_t>(pick.range(0, static_cast<std::int64_t>(level0.size()) - 1)));
            points.push_back(level0.point(index.back()));
        }
        for (const std::uint32_t width : {pg::max_batch, 1u, 13u}) {
            std::vector<float> out(points.size());
            pg::evaluate(*p, lod0, points, out, {.batch = width});
            int wrong = 0;
            for (std::size_t k = 0; k < points.size(); ++k) {
                wrong += std::bit_cast<std::uint32_t>(out[k]) != std::bit_cast<std::uint32_t>(a[index[k]]);
            }
            CHECK(wrong == 0);
        }

        // On the job system, with 1 and 4 threads: the same bits.
        for (const std::uint32_t threads : {1u, 4u}) {
            JobSystem jobs(threads);
            std::vector<float> parallel(level0.size());
            pg::evaluate(jobs, *p, lod0, level0, parallel);
            CHECK(bit_equal(parallel, a));
        }
        if (p == &caves) {
            const auto [lo, hi] = std::minmax_element(a.begin(), a.end());
            CHECK(*hi - *lo > 0.05f);  // not flat
        }
    }

    // Committed: a 32^3 box on the surface (ADR 0043: the macOS gate and Windows CI both check it).
    const std::vector<float> box =
        eval_box(caves, {.first = {surface_x, 1000, -2000}, .step = 1, .count = {32, 32, 32}});
    INFO(std::format("caves 0x{:016X}", hash_floats(box)));
    CHECK(hash_floats(box) == 0xB4F7287BFA292D1Dull);
}

TEST_CASE("[procgen.AC6] a program with two outputs writes them planar, each as its own program would") {
    const std::string two = R"({
      "veach_graph": 1, "id": "test:two", "inputs": { "p": "p_local" },
      "nodes": [
        { "id": "base", "type": "veach:fbm@1", "inputs": { "p": "p" }, "params": { "wavelength": { "m": 40 }, "octaves": 3 } },
        { "id": "mask", "type": "veach:clamp@1", "inputs": { "x": "base" }, "params": { "min": 0, "max": 0.5 } },
        { "id": "detail", "type": "veach:fbm@1", "inputs": { "p": "p" }, "params": { "wavelength": { "m": 5 }, "octaves": 2 } },
        { "id": "height", "type": "veach:blend@1", "inputs": { "a": "base", "b": "detail", "t": "mask" } }
      ],
      "outputs": { "height": "height", "mask": "mask" }
    })";
    const pg::FieldProgram both = program_of(two, {.seed = 5});
    REQUIRE(both.outputs.size() == 2);
    CHECK(both.outputs[0].name == "height");
    CHECK(both.outputs[1].name == "mask");
    CHECK(both.outputs[0].reg != both.outputs[1].reg);  // mask is read by height, and still kept to the end
    auto only = [&](std::string_view name) {
        std::string json = two;
        const std::string outputs = R"("outputs": { "height": "height", "mask": "mask" })";
        json.replace(json.find(outputs), outputs.size(), std::format(R"("outputs": {{ "{0}": "{0}" }})", name));
        return program_of(json, {.seed = 5});
    };
    const pg::LatticeBox box{.first = {surface_x, 0, 0}, .step = 2, .count = {20, 20, 20}};
    const std::vector<float> planar = eval_box(both, box);
    const std::vector<float> height = eval_box(only("height"), box);
    const std::vector<float> mask = eval_box(only("mask"), box);
    CHECK(bit_equal(std::span(planar).first(box.size()), height));
    CHECK(bit_equal(std::span(planar).last(box.size()), mask));
}

TEST_CASE("[procgen.AC10] the Luau builder makes the same program as the JSON file") {
    auto source = fs::read_file(test::source_dir() / "tests/procgen/terrain.graph.luau");
    REQUIRE(source.has_value());
    pg::Sandbox sandbox;
    auto built = sandbox.build_graph(*source, "terrain.graph.luau");
    REQUIRE_MESSAGE(built.has_value(), (built ? std::string() : built.error().message));
    CHECK(built->id() == "sp06:terrain");
    auto program = pg::compile(*built, {.seed = 42, .radius_planet_m = 6.0e6});
    REQUIRE(program.has_value());
    CHECK(program->words == terrain().words);

    // The builder's own errors carry the script's line; the graph's errors name the node, as from JSON.
    const auto twice = sandbox.build_graph(R"(return function(procgen)
        local g = procgen.graph("t:t")
        local p = g:input("p", "p_planet")
        g:node("n", "veach:fbm@1", { p = p, wavelength = procgen.planet_m(10), octaves = 1 })
        g:node("n", "veach:fbm@1", { p = p, wavelength = procgen.planet_m(10), octaves = 1 })
        return g
    end)",
                                           "twice.luau");
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().code == ErrorCode::procgen_graph_invalid);
    CHECK(twice.error().message.find("twice.luau:5:") != std::string::npos);
    const auto bare = sandbox.build_graph(R"(return function(procgen)
        local g = procgen.graph("t:t")
        g:output("h", g:node("n", "veach:fbm@1", { p = g:input("p", "p_planet"), wavelength = 10, octaves = 1 }))
        return g
    end)",
                                          "bare.luau");
    REQUIRE_FALSE(bare.has_value());
    CHECK(bare.error().code == ErrorCode::procgen_graph_invalid);
    CHECK(bare.error().message.find("node 'n'") != std::string::npos);
    CHECK(bare.error().hint.find("planet_m") != std::string::npos);
    // The generator rules hold in builders too (ADR 0070): no ^.
    const auto pow =
        sandbox.build_graph("return function(procgen) local x = 2 ^ 3 return procgen.graph('t:t') end", "pow.luau");
    REQUIRE_FALSE(pow.has_value());
    CHECK(pow.error().code == ErrorCode::script_compile_failed);
}

TEST_CASE("[procgen.AC12] lattice bounds and variations hold every evaluated value and every difference") {
    const pg::FieldProgram caves = program_of(caves_json, {.seed = 7});
    const pg::FieldProgram hills = program_of(terrain_json(), {.seed = 42, .planet_m_per_m = 1000.0});
    for (const pg::FieldProgram* p : {&caves, &hills}) {
        const std::vector<pg::FieldBound> bounds = pg::lattice_bounds(*p);
        REQUIRE(bounds.size() == 1);
        const pg::FieldBound& b = bounds[0];
        CHECK(b.lo < b.hi);
        CHECK(b.lipschitz > 0.0);
        CHECK(b.error > 0.0);
        CHECK(b.error < 1.0e-3 * (b.hi - b.lo));
        // Pairs of points 1 to 64 lattice units apart, around the surface of an Earth-sized body.
        HashStream pick = HashStream::make(11, "procgen.bounds", 0);
        std::vector<pg::LatticePoint> points;
        for (int k = 0; k < 4000; ++k) {
            const pg::LatticePoint a{surface_x + pick.range(-5000, 5000), pick.range(-5000, 5000),
                                     pick.range(-5000, 5000)};
            const std::int64_t step = pick.range(1, 64);
            points.push_back(a);
            points.push_back({a[0] + step, a[1] + (k % 3 == 0 ? step : 0), a[2]});
        }
        std::vector<float> out(points.size());
        pg::evaluate(*p, lod0, points, out);
        int outside = 0, steep = 0, varied = 0, loose = 0;
        double steepest = 0.0;
        for (std::size_t i = 0; i < points.size(); i += 2) {
            for (const std::size_t j : {i, i + 1}) {
                outside += out[j] < b.lo - b.error || out[j] > b.hi + b.error;
            }
            double d2 = 0.0;
            for (std::size_t a = 0; a < 3; ++a) {
                const double d = static_cast<double>(points[i + 1][a] - points[i][a]) * lod0.spacing_m;
                d2 += d * d;
            }
            const double slope =
                (std::fabs(static_cast<double>(out[i + 1]) - static_cast<double>(out[i])) - 2.0 * b.error) /
                std::sqrt(d2);
            steepest = std::max(steepest, slope);
            steep += slope > b.lipschitz;
            // The variation within that distance bounds the change too, and never exceeds the slope bound.
            const double variation = pg::lattice_variation(*p, std::sqrt(d2))[0];
            varied +=
                std::fabs(static_cast<double>(out[i + 1]) - static_cast<double>(out[i])) - 2.0 * b.error > variation;
            loose += variation > b.lipschitz * std::sqrt(d2) * (1.0 + 1e-12);
        }
        CHECK(outside == 0);
        CHECK(steep == 0);
        CHECK(varied == 0);
        CHECK(loose == 0);
        // Far beyond every wavelength the variation is the whole range, not the slope times the distance.
        CHECK(pg::lattice_variation(*p, 1.0e9)[0] <= (b.hi - b.lo) * (1.0 + 1e-9));
        // Conservative, but not absurdly so.
        INFO(std::format("steepest {} of bound {}", steepest, b.lipschitz));
        CHECK(steepest > 0.005 * b.lipschitz);
    }
}
