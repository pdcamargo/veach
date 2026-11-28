// Terrain materials (docs/specs/terrain-materials/spec.md): texture arrays, the layer library and its procedural
// fallback, the slot rules and their canonical weights, the toolbox's surfaces. CI-safe: no local assets are read
// (the scans live outside the repository); the golden image is tests/golden/terrain_materials.png.

#include <veach/asset/texture.hpp>
#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>
#include <veach/procgen/field_graph.hpp>
#include <veach/render/mesh_store.hpp>
#include <veach/runtime/terrain_surface.hpp>
#include <veach/space/gen/address.hpp>
#include <veach/space/realization.hpp>
#include <veach/space/surface.hpp>
#include <veach/voxel/density.hpp>
#include <veach/voxel/terrain.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <ostream>  // doctest prints string_view and string operands (MSVC needs it declared)
#include <string>

using namespace veach;

namespace {

namespace vs = veach::space;
namespace vx = veach::voxel;
namespace sg = ::space::gen;

std::string hex(std::uint64_t v) {
    return std::format("{:016x}", v);
}

std::uint64_t hash_weights(std::span<const std::array<float, 6>> weights) {
    std::uint64_t h = hash_id("terrain weights");
    for (const auto& w : weights) {
        for (const float f : w) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, 4);
            h = hash(h, bits);
        }
    }
    return h;
}

TextureAsset small_texture(TextureFormat format, std::uint8_t fill) {
    TextureAsset t;
    t.format = format;
    t.width = 8;
    t.height = 8;
    for (std::uint32_t m = 0; m < 4; ++m) {
        t.mips.emplace_back(texture_level_size(format, 8u >> m, 8u >> m), std::byte{fill});
    }
    return t;
}

constexpr float big = std::numeric_limits<float>::max();

}  // namespace

TEST_CASE("[terrain_materials.AC1] 2D arrays and float tables survive KTX2; layers of different shapes do not stack") {
    const std::array<TextureAsset, 3> layers{small_texture(TextureFormat::bc7_srgb, 1),
                                             small_texture(TextureFormat::bc7_srgb, 2),
                                             small_texture(TextureFormat::bc7_srgb, 3)};
    auto stacked = stack_layers(layers, "test");
    REQUIRE(stacked);
    CHECK(stacked->array);
    CHECK(stacked->layers == 3);
    CHECK(stacked->mips[0].size() == 3 * texture_level_size(TextureFormat::bc7_srgb, 8, 8));
    CHECK(stacked->mips[0][texture_level_size(TextureFormat::bc7_srgb, 8, 8)] == std::byte{2});  // layer 1 follows 0
    auto read = read_ktx2(write_ktx2(*stacked), "array");
    REQUIRE(read);
    CHECK(read->array);
    CHECK(read->layers == 3);
    CHECK(read->mips == stacked->mips);

    // A one-layer array stays an array (ADR 0090), and a 2D image stays 2D.
    auto one = stack_layers(std::span(layers).first(1), "one");
    REQUIRE(one);
    auto one_read = read_ktx2(write_ktx2(*one), "one");
    REQUIRE(one_read);
    CHECK(one_read->array);
    auto flat = read_ktx2(write_ktx2(layers[0]), "flat");
    REQUIRE(flat);
    CHECK_FALSE(flat->array);

    TextureAsset table;
    table.format = TextureFormat::rgba32_float;
    table.width = 4;
    table.height = 7;
    table.mips.emplace_back(4 * 7 * 16, std::byte{0x3F});
    auto table_read = read_ktx2(write_ktx2(table), "table");
    REQUIRE(table_read);
    CHECK(table_read->format == TextureFormat::rgba32_float);
    CHECK(table_read->mips == table.mips);

    const std::array<TextureAsset, 2> mixed{small_texture(TextureFormat::bc7_srgb, 1),
                                            small_texture(TextureFormat::bc7_unorm, 1)};
    auto refused = stack_layers(mixed, "mixed");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::asset_load_failed);

    // The MeshStore checks a runtime texture's levels.
    render::MeshStore store;
    CHECK(store.add_texture(*stacked));
    TextureAsset short_level = *stacked;
    short_level.mips[1].pop_back();
    CHECK_FALSE(store.add_texture(short_level));
}

TEST_CASE("[terrain_materials.AC2] without scans every layer is its procedural stand-in, and tiles divide 16 m") {
    JobSystem jobs(4);
    std::vector<TerrainLayerSource> sources(vs::surface_layers().begin(), vs::surface_layers().end());
    sources.front().folder = "/no/such/scans/grass";  // a missing folder is not an error
    auto library = load_terrain_library(sources, {}, jobs);
    REQUIRE(library);
    const TerrainLayerLibrary& l = **library;
    CHECK_FALSE(l.scanned());
    CHECK(l.layers().size() == sources.size());
    CHECK(l.albedo_height().array);
    CHECK(l.albedo_height().layers == sources.size());
    CHECK(l.albedo_height().width == 256);
    CHECK(l.albedo_height().format == TextureFormat::rgba8_srgb);
    CHECK(l.detail().format == TextureFormat::rgba8_unorm);
    CHECK(l.albedo_height().mips.size() == 9);  // 256 down to 1
    for (const TerrainLayerInfo& info : l.layers()) {
        INFO(info.name);
        CHECK(std::fmod(16.0f, info.tile_m) == doctest::Approx(0.0f));
        CHECK(info.mean_luminance > 0.0f);
        CHECK(info.mean_luminance < 1.0f);
    }
    CHECK(l.find("snow").has_value());
    CHECK_FALSE(l.find("lava field").has_value());
    CHECK(terrain_tile(3.0f) == doctest::Approx(16.0f / 5.0f));
    CHECK(terrain_tile(0.0f) == 2.0f);

    // A library config: paths from the base, fallbacks from the defaults by name.
    const auto config = read_terrain_library_config(
        R"({"scans": "quixel", "cache": "cache/t", "size": 512,
            "layers": [{"name": "snow", "scan": "fresh_snow"}, {"name": "odd", "fallback": {"roughness": 0.3}}]})",
        "/assets", vs::surface_layers());
    REQUIRE(config);
    CHECK(config->settings.size == 512);
    CHECK(config->settings.cache == std::filesystem::path("/assets/cache/t"));
    CHECK(config->layers.at(0).folder == std::filesystem::path("/assets/quixel/fresh_snow"));
    CHECK(config->layers.at(0).fallback.color.x == doctest::Approx(0.90f));  // the toolbox's snow
    CHECK(config->layers.at(1).fallback.roughness == doctest::Approx(0.3f));
    CHECK_FALSE(read_terrain_library_config("{\"layers\": [{}]}", "/"));
}

TEST_CASE("[terrain_materials.AC3] slot weights follow the rules, sum to 1, and are the same bits everywhere") {
    JobSystem jobs(2);
    auto library = load_terrain_library(vs::surface_layers(), {}, jobs);
    REQUIRE(library);
    TerrainSurfaceDesc desc{.name = "rules", .up = TerrainUp::y, .datum_m = 0.0};
    const TerrainWindow surface{-big, -big, 0.12f, 0.35f};
    desc.slots = {
        {.layer = 0, .rules = {{.slope = {-big, -big, 0.12f, 0.22f}, .depth = surface}}},
        {.layer = 2,
         .rules = {{.slope = {0.16f, 0.30f, big, big}, .depth = surface}, {.depth = {1.6f, 2.6f, big, big}}}},
        {.layer = 4, .rules = {{.depth = surface, .field0 = {-big, -big, -1.0f, 1.0f}, .strength = 3.0f}}},
        {.layer = 5, .rules = {{.depth = {0.08f, 0.3f, 1.8f, 2.8f}, .strength = 4.0f}}},
    };
    desc.fields = [](std::span<const math::dvec3> p, std::span<std::array<float, 2>> out) {
        for (std::size_t i = 0; i < p.size(); ++i) {
            out[i] = {static_cast<float>(10.0 - p[i].y), 0.5f};
        }
    };
    auto made = TerrainSurface::make(*library, desc);
    REQUIRE(made);
    const TerrainSurface& s = **made;
    const math::vec3 flat(0.0f, 1.0f, 0.0f);
    const math::vec3 steep = math::normalize(math::vec3(1.0f, 0.4f, 0.0f));  // 68 degrees
    const std::vector<TerrainVertexInput> v{
        {.position = {0, 0, 0}, .normal = flat},                // grass
        {.position = {0, 0, 0}, .normal = steep},               // cliff
        {.position = {0, 20, 0}, .normal = flat},               // snow (field0 = -10)
        {.position = {0, 0, 0}, .normal = flat, .depth = 1.0},  // soil in a dig
        {.position = {0, 0, 0}, .normal = flat, .depth = 4.0},  // rock deep in a dig
    };
    std::vector<std::array<float, 6>> w(v.size());
    s.weights(v, w);
    CHECK(w[0][0] == doctest::Approx(1.0f));
    CHECK(w[1][1] == doctest::Approx(1.0f));
    CHECK(w[2][2] > 0.7f);
    CHECK(w[3][3] == doctest::Approx(1.0f));
    CHECK(w[4][1] == doctest::Approx(1.0f));
    for (const auto& x : w) {
        CHECK(x[0] + x[1] + x[2] + x[3] + x[4] + x[5] == doctest::Approx(1.0f));
    }

    // The toolbox's surface on the slice planet at s = 0.01: its climate fields (a procgen program, canonical) and
    // weights at 400 points on the sphere, hashed. The same bits on macOS and Windows CI.
    auto system =
        vs::realize({.seed = 42, .system = sg::parse_system_address("S:1000400100008000")->value, .scale = 0.01});
    REQUIRE(system);
    const vs::RealizedBody& home = *system->home();
    CHECK(vs::biome_of(*system, home) == vs::Biome::temperate);
    auto home_desc = vs::surface_desc(*system, home, **library, vs::SurfaceColors::automatic);
    REQUIRE(home_desc);
    CHECK_FALSE(home_desc->slots.front().use_palette);  // a habitable planet keeps its scans' colors
    auto home_surface = TerrainSurface::make(*library, *home_desc);
    REQUIRE(home_surface);
    std::vector<TerrainVertexInput> points;
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 20; ++j) {
            const double lat = -1.4 + 2.8 * i / 19.0;
            const double lon = 6.2831853 * j / 20.0;
            const math::dvec3 n(dmath::cos(lat) * dmath::cos(lon), dmath::sin(lat), dmath::cos(lat) * dmath::sin(lon));
            // Positions as the lattice gives them: whole 1/65536 of half a meter.
            math::dvec3 p = n * (home.radius + home.relief * 0.3 * (i % 4));
            p = math::round(p * 131072.0) / 131072.0;
            points.push_back({.position = p, .normal = math::vec3(n), .depth = (j % 7 == 0) ? 1.0 : 0.0});
        }
    }
    std::vector<std::array<float, 6>> home_w(points.size());
    (*home_surface)->weights(points, home_w);
    std::array<float, 6> totals{};
    for (const auto& x : home_w) {
        for (std::size_t k = 0; k < 6; ++k) {
            totals[k] += x[k];
        }
    }
    INFO("weights " << hex(hash_weights(home_w)) << " totals " << totals[0] << " " << totals[1] << " " << totals[2]
                    << " " << totals[3] << " " << totals[4] << " " << totals[5]);
    CHECK(totals[0] > 20.0f);  // grass in the temperate lowlands
    CHECK(totals[4] > 5.0f);   // snow toward the poles and on the heights
    CHECK(totals[5] > 20.0f);  // soil in the digs
    CHECK(hash_weights(home_w) == 0x207ff914eab968eaULL);
}

TEST_CASE(
    "[terrain_materials.AC4] the toolbox paints alien worlds from palettes, by default or when the creator asks") {
    JobSystem jobs(2);
    auto library = load_terrain_library(vs::surface_layers(), {}, jobs);
    REQUIRE(library);
    auto system =
        vs::realize({.seed = 42, .system = sg::parse_system_address("S:1000400100008000")->value, .scale = 0.01});
    REQUIRE(system);
    const vs::RealizedBody& home = *system->home();
    auto palette = vs::surface_desc(*system, home, **library, vs::SurfaceColors::palette);
    REQUIRE(palette);
    CHECK(palette->slots.front().use_palette);
    CHECK(palette->slots.size() == 6);
    // A moon without air: airless, in palette colors unless the creator asks for the scans'.
    for (const vs::RealizedBody& b : system->bodies) {
        if (b.parent != home.index || !b.solid) {
            continue;
        }
        CHECK(vs::biome_of(*system, b) == vs::Biome::airless);
        auto moon = vs::surface_desc(*system, b, **library, vs::SurfaceColors::automatic);
        REQUIRE(moon);
        CHECK(moon->slots.front().use_palette);
        auto scanned = vs::surface_desc(*system, b, **library, vs::SurfaceColors::scanned);
        REQUIRE(scanned);
        CHECK_FALSE(scanned->slots.front().use_palette);
    }
    CHECK(vs::parse_surface_colors("") == vs::SurfaceColors::automatic);
    CHECK(vs::parse_surface_colors("palette") == vs::SurfaceColors::palette);
    auto bad = vs::parse_surface_colors("alien");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == ErrorCode::universe_settings_invalid);
}

TEST_CASE("[terrain_materials.AC3] a chunk's vertices: lattice positions, and depth only where an edit touches") {
    JobSystem jobs(2);
    auto graph = procgen::parse_field_graph(R"({
      "veach_graph": 1, "id": "test:hills", "inputs": { "q": "p_local" },
      "nodes": [ { "id": "h", "type": "veach:fbm@1", "inputs": { "p": "q" },
                   "params": { "wavelength": { "m": 40 }, "octaves": 3 } } ],
      "outputs": { "h": "h" } })",
                                            "test");
    REQUIRE(graph);
    auto program = procgen::compile(*graph, {.seed = 7});
    REQUIRE(program);
    auto density = vx::ProgramDensity::make(
        std::move(*program), {.shape = vx::BaseShape::plane, .density_output = "h", .density_scale = -4.0});
    REQUIRE(density);
    vx::TerrainSettings settings;
    settings.octree = vx::OctreeBounds::around_origin(128.0);
    settings.lambda = 2.0;
    settings.asynchronous = false;
    settings.budget.jobs_per_frame = 100'000;
    auto body = std::make_shared<vx::TerrainBody>(*density, settings);
    const double ground = -(*density)->distance(0.0, 0.0, 0.0);
    REQUIRE(body->apply({.kind = vx::BrushKind::subtract,
                         .center_mm = {0, static_cast<std::int64_t>(ground * 1000.0) - 1000, 0},
                         .radius_mm = 2000},
                        jobs));
    const std::vector<std::array<double, 3>> viewers{{0.0, ground + 2.0, 0.0}};
    body->set_viewers(viewers);
    for (int i = 0; i < 8 && body->drawn().empty(); ++i) {
        static_cast<void>(body->update(jobs));
    }
    REQUIRE_FALSE(body->drawn().empty());
    std::size_t deep = 0, vertices = 0;
    for (const vx::ChunkRef& r : body->drawn()) {
        const auto inputs = terrain_vertex_inputs(*r, body.get());
        REQUIRE(inputs.size() == r->mesh.position.size());
        for (std::size_t v = 0; v < inputs.size(); ++v) {
            ++vertices;
            deep += inputs[v].depth > 0.5 ? 1 : 0;
            if (!body->edits().touches(vx::sample_box(r->slot.key))) {
                CHECK(inputs[v].depth == 0.0);
            }
        }
        const auto none = terrain_vertex_inputs(*r, nullptr);
        CHECK(std::ranges::all_of(none, [](const TerrainVertexInput& in) { return in.depth == 0.0; }));
    }
    INFO(deep << " of " << vertices << " vertices more than 0.5 m below the generated ground");
    CHECK(deep > 0);  // the hole's walls
}
