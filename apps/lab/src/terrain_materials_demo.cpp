// The lab's terrain materials scene (docs/specs/terrain-materials/spec.md, terrain_materials.AC5): a flat world of
// hills and cliffs streamed by engine/voxel, drawn with TerrainMaterial over the procedural layer library (no scans:
// the golden never depends on local assets), with a dug hole whose walls show the subsurface layers.
//   veach-lab --rhi metal --headless --frames 4 --script scripts/terrain_materials.luau --screenshot out.png

#include "terrain_materials_demo.hpp"

#include <veach/core/jobs.hpp>
#include <veach/procgen/field_graph.hpp>
#include <veach/runtime/terrain.hpp>
#include <veach/runtime/terrain_surface.hpp>
#include <veach/space/surface.hpp>
#include <veach/voxel/density.hpp>
#include <veach/voxel/terrain.hpp>

#include <limits>
#include <memory>

namespace lab {

namespace {

namespace vx = veach::voxel;
namespace math = veach::math;

constexpr float big = std::numeric_limits<float>::max();

/// Hills 12 m high over 60 m, with a ridged band that stands up in cliffs.
constexpr std::string_view hills_graph = R"({
  "veach_graph": 1, "id": "lab:terrain_materials", "inputs": { "q": "p_local" },
  "nodes": [
    { "id": "hills", "type": "veach:fbm@1", "inputs": { "p": "q" },
      "params": { "wavelength": { "m": 70 }, "octaves": 5 } },
    { "id": "ridge", "type": "veach:ridge@1", "inputs": { "x": "hills" }, "params": { "offset": 1.0 } },
    { "id": "height", "type": "veach:blend@1", "inputs": { "a": "hills", "b": "ridge", "t": "ridge" } }
  ],
  "outputs": { "height": "height" }
})";

struct Demo {
    std::shared_ptr<vx::TerrainBody> body;
};

void build(veach::World world, const TerrainMaterialsDemo& demo, Demo& state) {
    auto graph = veach::procgen::parse_field_graph(hills_graph, "lab:terrain_materials");
    auto program = graph ? veach::procgen::compile(*graph, {.seed = 0x7E11})
                         : veach::Result<veach::procgen::FieldProgram>(std::unexpected(graph.error()));
    if (!program) {
        veach::log::error("lab", program.error());
        return;
    }
    auto density = vx::ProgramDensity::make(
        std::move(*program),
        {.shape = vx::BaseShape::plane, .height_m = 0.0, .density_output = "height", .density_scale = -10.0});
    if (!density) {
        veach::log::error("lab", density.error());
        return;
    }
    veach::Services& services = veach::services(world);
    auto library = veach::load_terrain_library(veach::space::surface_layers(), {}, services.get<veach::JobSystem>());
    if (!library) {
        veach::log::error("lab", library.error());
        return;
    }
    const auto layer = [&](std::string_view name) {
        return (*library)->find(name).value_or(0);
    };
    const veach::TerrainWindow surface{-big, -big, 0.12f, 0.35f};
    veach::TerrainSurfaceDesc desc{.name = "lab hills", .up = veach::TerrainUp::y, .datum_m = 0.0};
    desc.slots = {
        {.layer = layer("grass"),
         .rules = {{.slope = {-big, -big, 0.12f, 0.22f}, .depth = surface, .field1 = {0.35f, 0.45f, big, big}}},
         .palette = {0.30f, 0.05f, 0.12f}},
        {.layer = layer("beach_sand"),
         .rules = {{.slope = {-big, -big, 0.12f, 0.22f}, .depth = surface, .field1 = {-big, -big, 0.35f, 0.45f}}},
         .palette = {0.45f, 0.25f, 0.12f}},
        {.layer = layer("cliff"),
         .rules = {{.slope = {0.16f, 0.30f, big, big}, .depth = surface}, {.depth = {1.6f, 2.6f, big, big}}},
         .palette = {0.10f, 0.11f, 0.16f}},
        {.layer = layer("snow"),
         .rules = {{.slope = {-big, -big, 0.25f, 0.38f},
                    .depth = surface,
                    .field0 = {-big, -big, -1.0f, 1.0f},
                    .strength = 3.0f}},
         .palette = {0.85f, 0.80f, 0.90f}},
        {.layer = layer("soil"),
         .rules = {{.depth = {0.08f, 0.3f, 1.8f, 2.8f}, .strength = 4.0f}},
         .palette = {0.12f, 0.06f, 0.04f}},
    };
    for (veach::TerrainSlot& slot : desc.slots) {
        slot.use_palette = demo.palette;
    }
    // Temperature falls with height (snow on the tops), moisture varies across x.
    desc.fields = [](std::span<const math::dvec3> positions, std::span<std::array<float, 2>> out) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const math::dvec3& p = positions[i];
            out[i] = {static_cast<float>(14.0 - 1.7 * p.y), static_cast<float>(0.5 + p.x / 60.0)};
        }
    };
    auto surface_made = veach::TerrainSurface::make(*library, std::move(desc));
    if (!surface_made) {
        veach::log::error("lab", surface_made.error());
        return;
    }
    auto material = (*surface_made)->material(services.get<veach::render::MeshStore>());
    if (!material) {
        veach::log::error("lab", material.error());
        return;
    }
    vx::TerrainSettings settings;
    settings.octree = vx::OctreeBounds::around_origin(1024.0);
    settings.lambda = 4.0;
    settings.asynchronous = false;  // every chunk meshed in the frame that asks: the image never depends on timing
    settings.budget.jobs_per_frame = 100'000;
    state.body = std::make_shared<vx::TerrainBody>(*density, settings);
    if (demo.dig) {
        // A hole 3 m across in the hillside in front of the camera, before the first chunk is meshed.
        const double x = demo.dig_x, z = demo.dig_z;
        const double ground = -(*density)->distance(x, 0.0, z);  // plane: d = y - h, so h = -d(x, 0, z)
        auto op = vx::BrushOp{
            .kind = vx::BrushKind::subtract,
            .center_mm = {static_cast<std::int64_t>(x * 1000.0), static_cast<std::int64_t>((ground - 0.5) * 1000.0),
                          static_cast<std::int64_t>(z * 1000.0)},
            .radius_mm = 2'500};
        if (auto r = state.body->apply(op, services.get<veach::JobSystem>()); !r) {
            veach::log::error("lab", r.error());
        }
    }
    if (veach::Entity camera = world.lookup("camera"); camera.is_valid()) {
        camera.add<veach::TerrainViewer>();
    } else {
        veach::log::error("lab", "the terrain materials scene streams around an entity named \"camera\"; set it first");
    }
    services.get<veach::TerrainBodies>().add(veach::Entity{}, state.body,
                                             {.materials = {*material}, .surface = *surface_made});
}

}  // namespace

void register_terrain_materials_demo(veach::World& world) {
    world.component<TerrainMaterialsDemo>()
        .member(&TerrainMaterialsDemo::palette, "palette")
        .member(&TerrainMaterialsDemo::dig, "dig")
        .member(&TerrainMaterialsDemo::dig_x, "dig_x")
        .member(&TerrainMaterialsDemo::dig_z, "dig_z")
        .add<veach::Scriptable>();
    auto state = std::make_shared<Demo>();
    world.observer<const TerrainMaterialsDemo>("BuildTerrainMaterialsDemo")
        .event(veach::OnSet)
        .each([state](veach::Entity e, const TerrainMaterialsDemo& demo) {
            if (state->body == nullptr) {
                build(e.world(), demo, *state);
            }
        });
}

}  // namespace lab
