# Terrain materials: scanned layers, rules and palettes

Voxel terrain draws with up to six surface layers per body, placed by rules you write and blended per pixel
([spec](../specs/terrain-materials/spec.md), [ADR 0098](../adr/0098-terrain-materials.md)). Three pieces:

| | What | Where |
|---|---|---|
| Layer library | the game's surfaces (grass, rock, sand...), cooked to two BC7 texture arrays | `veach/runtime/terrain_surface.hpp` |
| Surface | one body's slots: a layer, its rules, its color mode | `veach/runtime/terrain_surface.hpp` |
| TerrainMaterial | the shader that blends them | `engine/render/shaders/terrain/terrain_material.slang` |

The space toolbox builds surfaces for its planets itself (`veach/space/surface.hpp`); a game on the toolbox only
supplies the library.

## Register the material

```cmake
veach_add_terrain_material(my-app)               # before veach_add_scene_shaders
veach_add_scene_shaders(my-app NAMESPACE my_shaders)
```

An app that draws a surface without registering it logs `material kind 'TerrainMaterial' is not registered`.

## A layer library

```json
{
  "scans": "quixel", "cache": "cache/terrain-layers", "size": 2048,
  "layers": [
    { "name": "grass", "scan": "Grass_Wild_pjwbe0_2K_surface_ms", "tile_m": 2.0 },
    { "name": "cliff", "scan": "granite_rock_face_vissbgb_2k", "tile_m": 4.0,
      "fallback": { "color": [0.48, 0.46, 0.43], "roughness": 0.75, "relief": 1.0, "grain_m": 0.6 } }
  ]
}
```

```cpp
auto config = veach::read_terrain_library_config(json, local_assets_dir, veach::space::surface_layers());
auto library = veach::load_terrain_library(config->layers, config->settings, jobs);
veach::services(world).provide<veach::TerrainLayers>(layers);   // {library}: the toolbox's UniverseModule uses it
```

- **Scans.** A folder per layer; files are found by their names' endings: `_BaseColor` or `_Albedo`, `_Normal`,
  `_Roughness` (or `_Gloss`), `_AO`, `_Displacement` (or `_Bump`), JPEG or PNG. Color and normal are required. A
  Megascans JSON next to them gives the tile (`scanArea`) unless `tile_m` does. Normal maps of either green convention
  work: the cooker checks them against the height map.
- **Tiles** are rounded to 16 / n m (16, 8, 5.33, 4, 3.2, 2.67, 2, ... 0.25): chunk origins are multiples of 16 m, so
  such a tile continues across every chunk border.
- **The cache** holds each cooked layer as two KTX2 files and a JSON of its means, keyed by the sources' names, sizes
  and times and the size. 15 layers at 2048 cook in about 4 s on the M5 Max (release); a cache hit decodes nothing.
- **Fallbacks.** A missing or incomplete folder is a procedural layer (tileable noise height, normal, AO, the fallback
  color); a library with no scans at all is 256x256 RGBA8. Layers without a `fallback` take the one of their name in
  the defaults you pass (the toolbox's roles).

### The slice's library

`apps/slice/terrain_layers.json` maps the toolbox's roles to the Quixel Megascans surfaces the owner downloaded from
Fab. They are **never committed**: they live in `local-assets/quixel/` at the repository root (gitignored; manifest in
its README), and the cache in `local-assets/cache/`. `VEACH_LOCAL_ASSETS=/path/to/local-assets` points the slice
elsewhere (a worktree uses the main checkout's). Without the folder the slice draws the procedural stand-ins.

## A surface

```cpp
using W = veach::TerrainWindow;  // {lo0, lo1, hi0, hi1}: 0 below lo0, 1 from lo1 to hi0, 0 above hi1
constexpr float big = std::numeric_limits<float>::max();
veach::TerrainSurfaceDesc desc{.name = "hills", .up = veach::TerrainUp::y, .datum_m = 0.0};
desc.slots = {
    {.layer = *library->find("grass"), .rules = {{.slope = W{-big, -big, 0.12f, 0.22f}}}},
    {.layer = *library->find("cliff"), .rules = {{.slope = W{0.16f, 0.30f, big, big}},      // steep ground ...
                                                 {.depth = W{1.6f, 2.6f, big, big}}}},   // ... or deep in a dig
    {.layer = *library->find("soil"), .rules = {{.depth = W{0.08f, 0.3f, 1.8f, 2.8f}, .strength = 4.0f}},
     .use_palette = true, .palette = {0.12f, 0.06f, 0.04f}},
};
desc.fields = [](std::span<const veach::math::dvec3> p, std::span<std::array<float, 2>> out) { /* thread-safe */ };
auto surface = veach::TerrainSurface::make(library, desc);
terrain.add(frame, body, {.materials = {*(*surface)->material(store)}, .surface = *surface});
```

- **Inputs.** Altitude above the datum (`radial`: |p| − datum; `y`: y − datum), slope = 1 − cos(angle from up) (0.29 at
  45°, 0.5 at 60°), depth below the generated surface (non-zero only in chunks an edit touches), and the two fields
  your function returns per vertex (body-frame positions in, called on chunk jobs). A slot's weight is the largest of
  its rules, each the product of its windows times its strength; the six weights are normalized per vertex.
- **Keep it canonical** if your game hashes or replays anything that depends on it: fields from procgen programs and
  + − × ÷ √ only. The weights are then the same bits everywhere (`[terrain_materials.AC3]` commits a hash).
- **Color modes.** By default a slot shows its scan's color. `use_palette` paints `palette` (linear RGB) times the
  scan's luminance over its mean (`palette_contrast` scales it): the scan's light and shade in the planet's color.
- **Look settings** on the desc: `macro_strength` (large-scale brightness variation), `height_blend` (how much the
  layers' heights decide their borders), `triplanar_sharpness`, `fade_start` / `fade_end` (detail fades by the pixel's
  footprint relative to each tile), `ao_strength`, `normal_scale`.

## What the shader does

Per pixel: read the six weights; per layer with weight, sample both arrays triplanar in the chunk's object space (the
body frame) at a near tile and a far one (about 2.7 times larger), varying the near one by the far one; past each
scale's fade, the layer's mean color and roughness stand in (no texture read). Then height blending (weight plus
height near the best wins: sand fills the cracks between stones), a low-frequency noise in the body frame against
tiling, AO into the color, and the bumps added to the vertex normal. From orbit only the means remain, so a planet
shows the colors of its ground.

## The space toolbox's surfaces

`space::surface_desc(system, body, library, colors)` picks a biome set from the body (temperate on potentially
habitable planets; arid, cold or airless otherwise), places its roles (`space::surface_layers()`: grass, beach_sand,
cliff, rocky_ground, snow, soil, red_sand, cracked, basalt, lava, salt, sandstone, ice, gravel, sand) with climate
fields (temperature in °C from the body's mean, latitude and height; moisture from continental noise), and colors:
the scans' on habitable planets, palettes from the star and the body's rock elsewhere. The creator overrides it:

```lua
veach.spawn("universe"):set(c.Universe, { seed = "42", system = "S:1000400100008000", scale = 0.01,
                                          surface_colors = "palette" })   -- "" (auto), "scanned", "palette"
```

## Checking your terrain

- `veach-lab --rhi metal --headless --frames 4 --script scripts/terrain_materials.luau --screenshot out.png`: the
  golden scene (procedural layers, a dug hole).
- `VEACH_SLICE_AUTOPILOT=1 VEACH_SLICE_SHOTS=out veach-slice --script scripts/slice_s1.luau --rhi metal --headless
  --frames 40000`: the flight, with shots at 10 km and of a cliff close-up; `scripts/slice_alien_s1.luau` in palettes.
- The log's `frame times at ...` lines give the `shade` pass, where TerrainMaterial runs.
