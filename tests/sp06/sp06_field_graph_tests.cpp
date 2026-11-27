// Spike SP-06 (VCH-082): one field graph, three evaluators (docs/research/spikes/sp-06-field-graph.md).

#include "sp06/field_gpu.hpp"
#include "sp06/field_graph.hpp"
#include "support/gpu_device.hpp"

#include <veach/core/hash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace veach;

namespace {

sp06::Program terrain(std::uint64_t seed = 42) {
    auto json = sp06::terrain_graph_json();
    REQUIRE_MESSAGE(json.has_value(), (json ? std::string() : json.error().message));
    auto program = sp06::compile(*json, {.seed = seed, .radius_planet_m = 6.0e6});
    REQUIRE_MESSAGE(program.has_value(), (program ? std::string() : program.error().message));
    return std::move(*program);
}

/// The ops of a program as (opcode, payload) pairs.
std::vector<std::pair<sp06::Op, std::vector<std::uint32_t>>> ops(const sp06::Program& p) {
    std::vector<std::pair<sp06::Op, std::vector<std::uint32_t>>> out;
    for (std::size_t pc = 0; pc < p.words.size(); pc += sp06::op_header_words + p.words[pc + 5]) {
        const auto* payload = p.words.data() + pc + sp06::op_header_words;
        out.emplace_back(static_cast<sp06::Op>(p.words[pc]), std::vector(payload, payload + p.words[pc + 5]));
    }
    return out;
}

Error compile_error(std::string_view json) {
    auto program = sp06::compile(json, {});
    REQUIRE_FALSE(program.has_value());
    return program.error();
}

std::uint64_t hash_floats(const std::vector<float>& values) {
    std::uint64_t h = 0;
    for (const float v : values) {
        h = mix64(h ^ std::bit_cast<std::uint32_t>(v));
    }
    return h;
}

float max_abs_difference(const std::vector<float>& a, const std::vector<float>& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }
    return worst;
}

}  // namespace

TEST_CASE("[sp06.AC1] the terrain graph compiles to one op list with folded constants and hashed seeds") {
    const sp06::Program p = terrain();
    const auto list = ops(p);
    REQUIRE(list.size() == 8);  // every node is reachable from the output
    CHECK(p.noise_octaves == 11);
    CHECK(p.registers <= 4);
    CHECK(list[0].first == sp06::Op::fbm);
    // continents: 6 octaves; wavelength 3,000 km on a 6,000 km planet folds to frequency 2 on the unit sphere.
    CHECK(list[0].second[0] == 6);
    CHECK(std::bit_cast<float>(list[0].second[1]) == 2.0f);
    // The seeds sp06_field_handwritten.slang bakes in: veach::hash(hash(42, hash_id(node)), octave), folded.
    const std::array<std::uint32_t, 6> continents{0x0D1C6AB3u, 0x2CA4B71Cu, 0x95510E19u,
                                                  0x376320F1u, 0x9BE8AC0Eu, 0x8413A8EDu};
    CHECK(std::equal(continents.begin(), continents.end(), list[0].second.begin() + 5));
    const auto mountains =
        std::find_if(list.begin() + 1, list.end(), [](const auto& op) { return op.first == sp06::Op::fbm; });
    REQUIRE(mountains != list.end());
    CHECK(std::bit_cast<float>(mountains->second[1]) == 10.0f);
    CHECK(mountains->second[5] == 0x561C8B05u);
    CHECK(mountains->second[9] == 0xC3058AD7u);
    // Another body seed gives other octave seeds and nothing else.
    const auto other = ops(terrain(43));
    CHECK(other[0].second[5] != list[0].second[5]);
    CHECK(std::equal(other[0].second.begin(), other[0].second.begin() + 5, list[0].second.begin()));
}

TEST_CASE("[sp06.AC1] field graph errors name the node and the fix") {
    const std::string head = R"({"veach_graph": 1, "id": "t", "inputs": {"p": "p_planet"}, "outputs": {"h": "n"}, )";
    const Error bare = compile_error(head + R"("nodes": [{"id": "n", "type": "veach:fbm@1", "inputs": {"p": "p"},
                            "params": {"wavelength": 1000, "octaves": 2}}]})");
    CHECK(bare.code == ErrorCode::data_type_mismatch);
    CHECK(bare.message.find("'n'") != std::string::npos);
    CHECK(bare.hint.find("planet_m") != std::string::npos);

    const Error version =
        compile_error(head + R"("nodes": [{"id": "n", "type": "veach:fbm@2", "inputs": {"p": "p"}, "params": {}}]})");
    CHECK(version.hint.find("veach:fbm@1") != std::string::npos);

    const Error param = compile_error(head + R"("nodes": [{"id": "n", "type": "veach:fbm@1", "inputs": {"p": "p"},
                            "params": {"wavelength": {"planet_m": 1000}, "octaves": 2, "persistence": 0.5}}]})");
    CHECK(param.code == ErrorCode::data_unknown_field);

    const Error cycle =
        compile_error(head + R"("nodes": [{"id": "n", "type": "veach:add@1", "inputs": {"a": "m", "b": "m"}},
                                                          {"id": "m", "type": "veach:mul@1", "inputs": {"a": "n", "b": "n"}}]})");
    CHECK(cycle.message.find("cycle") != std::string::npos);

    const Error position =
        compile_error(head + R"("nodes": [{"id": "n", "type": "veach:ridge@1", "inputs": {"x": "p"}}]})");
    CHECK(position.hint.find("node's output") != std::string::npos);
}

TEST_CASE(
    "[sp06.AC2] the CPU batch interpreter is canonical: batches equal single samples, and the hash is committed") {
    const sp06::Program p = terrain();
    sp06::FaceRegion region;
    region.resolution = 128;
    std::vector<float> batch(std::size_t{128} * 128);
    std::vector<float> scalar(batch.size());
    sp06::evaluate_batch(p, region, batch);
    sp06::evaluate_scalar(p, region, scalar);
    CHECK(std::memcmp(batch.data(), scalar.data(), batch.size() * sizeof(float)) == 0);
    const auto [lo, hi] = std::minmax_element(batch.begin(), batch.end());
    CHECK(*lo >= 0.0f);
    CHECK(*hi <= 1.0f);
    CHECK(*hi - *lo > 0.5f);  // not flat
    // ADR 0043: the same bits on macOS arm64 (Apple Clang) and Windows x64 (MSVC), Debug and Release.
    CHECK(hash_floats(batch) == 0xFB7136EA481F1F8Dull);
}

TEST_CASE("[sp06.AC2] without hardware FMA, the double-precision path gives fma's bits") {
    // field_graph.cpp's fused() and the HLSL path of sp06_field_noise.slang round a * b + c through double when
    // there is no float FMA (MSVC x64, D3D). Check that claim here, where both exist, on the operands they get.
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

TEST_CASE("[sp06.AC3] the GPU interpreter and the hand-written kernel agree with the CPU interpreter within 1e-5") {
    auto device = test::gpu_device();
    if (!device) {
        return;
    }
    const sp06::Program p = terrain();
    sp06::FaceRegion region;
    region.resolution = 256;
    std::vector<float> cpu(std::size_t{256} * 256);
    sp06::evaluate_batch(p, region, cpu);
    auto gpu = sp06::run_gpu(*device, p, region, 2);
    REQUIRE_MESSAGE(gpu.has_value(), (gpu ? std::string() : gpu.error().message));
    const float interpreted = max_abs_difference(gpu->interpreted, cpu);
    const float handwritten = max_abs_difference(gpu->handwritten, cpu);
    MESSAGE("max |GPU interpreter - CPU| = ", interpreted, ", max |hand-written - CPU| = ", handwritten);
    CHECK(interpreted < 1e-5f);
    CHECK(handwritten < 1e-5f);
    CHECK(max_abs_difference(gpu->interpreted, gpu->handwritten) < 1e-5f);
    CHECK((gpu->interpreted_ms.size() == 2 || !gpu->timing_unavailable.empty()));
}
