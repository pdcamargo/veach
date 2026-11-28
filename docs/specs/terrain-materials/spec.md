# Terrain materials: scanned PBR layers, placed by the generator, with opt-in procedural color

**Status:** Implemented
**Area:** render, runtime, space
**Owner:** dev lead

## Summary

Voxel terrain is drawn with up to six surface layers per body (grass, rock, sand, snow, soil...), each a scanned PBR
set (color, normal, roughness, AO, height) in BC7 texture arrays, placed per vertex by rules over slope, altitude,
depth below the generated surface and two climate fields from the generator, and blended per pixel by the layers'
heights, triplanar, at two scales, with macro variation and a fade to each layer's mean color with distance. A layer
can keep its scan's color or take a palette color with the scan's detail (the opt-in procedural albedo), so alien
worlds get alien palettes. Games bring their own scans; without them every layer is a procedural stand-in.

## Motivation

The slice drew its planet with three flat colors by voxel material id: from orbit the planet showed blocky material
cells, on the ground nothing had texture, and a dug hole was the same green as the meadow. Item 7 of the renderer pass,
the one that makes the slice look like a game (VCH-130). The prototype uses Quixel Megascans surfaces from Fab, which
may not be redistributed: they stay outside the repository (`local-assets/quixel/`, gitignored), so the engine, its
tests and CI must work without them.

## Requirements

- A layer library MUST cook scanned sets (files ending in `_BaseColor`/`_Albedo`, `_Normal`, `_Roughness` or
  `_Gloss`, `_AO`, `_Displacement` or `_Bump`, JPEG or PNG) into two 2D arrays: albedo (sRGB) with height in alpha,
  and normal xy, roughness and AO; BC7 with full mip chains, built in parallel on the job system, with normals
  renormalized and roughness widened by the normals' spread (Toksvig) at each mip.
- Normal maps in either green convention MUST come out right: the cooker checks them against the height map's slopes.
- Cooked layers MUST be cached (KTX2, keyed by the sources' names, sizes and times, the size and the cook version) in a
  directory the game names; a cache hit MUST NOT decode or encode anything.
- A layer whose folder is missing or incomplete MUST become a procedural layer (tileable noise height, its normal,
  roughness and AO, a fallback color) without an error; with no scan at all the library MUST be 256x256 RGBA8, cheap
  enough for CI and Debug builds.
- A tile MUST be 16 / n m (whole n): chunk origins are multiples of 16 m, so tiles continue across chunks exactly.
- A body's surface MUST have 1 to 6 slots, each a library layer, one or more rules (the largest wins) and a color mode
  (the scan's, or a palette color times the scan's luminance relative to its mean).
- A rule MUST be a product of smooth windows over altitude above the datum, slope (1 - cos of the angle from up), depth
  below the generated surface, and two game-supplied fields, times a strength.
- Each chunk vertex MUST carry its six slot weights (summing to 1) in its tangent and uv, computed on the chunk's job
  from the body-frame position given by the lattice, so shared vertices of neighboring chunks get the same weights and
  the weights are canonical (+ - * / sqrt only, fields from canonical sources).
- Depth below the generated surface MUST be computed only for chunks an edit touches (the density source's
  generated distance at the vertex), so a dig's walls show the subsurface layers and untouched chunks cost nothing.
- The shader MUST blend layers by weight plus height (height blending), project triplanar in the chunk's object space
  (the body frame), sample each layer at a near and a far tile and vary the near tile by the far one, filter by the
  pixel footprint (explicit gradients, anisotropic 8x), and past each scale's fade replace it with the layer's mean
  color and roughness, so the far terrain and the planet from orbit show the same colors as the ground.
- A low-frequency noise in the body frame (continuous across chunks through the draw's material origin) MUST vary
  brightness and roughness against tiling.
- The space toolbox MUST pick a biome set per body (temperate, arid, cold, airless) from its composition and climate,
  its climate fields (temperature, moisture) from a procgen program in planet meters, and its colors: the scans' on
  potentially habitable planets, palettes from the star and the body elsewhere; the creator MAY force either
  (`Universe.surface_colors`).
- Apps opt in to the material (`veach_add_terrain_material`); apps without terrain keep their pipelines.

## API sketch

```cpp
// engine/runtime: veach/runtime/terrain_surface.hpp
auto config = veach::read_terrain_library_config(json, local_assets, veach::space::surface_layers());
auto library = veach::load_terrain_library(config->layers, config->settings, jobs);   // scans, cache, fallbacks
veach::services(world).provide<veach::TerrainLayers>(layers);                         // for UniverseModule

veach::TerrainSurfaceDesc desc{.up = veach::TerrainUp::radial, .datum_m = radius, .fields = climate};
desc.slots.push_back({.layer = *library->find("cliff"),
                      .rules = {{.slope = {0.16f, 0.30f, big, big}}, {.depth = {1.8f, 3.0f, big, big}}}});
auto surface = veach::TerrainSurface::make(library, desc);
terrain.add(frame, body, {.materials = {*(*surface)->material(store)}, .surface = *surface});
```

```lua
veach.spawn("universe"):set(c.Universe, { seed = "42", system = "S:1000400100008000", scale = 0.01,
                                          surface_colors = "palette" })   -- "", "scanned" or "palette"
```

Renderer additions: `MeshStore::add_texture` (runtime textures, 2D arrays included), `MaterialAsset::array_textures`,
`custom`, `max_anisotropy`, `MeshDraw::material_origin`, and `SurfaceInput`'s object-space position, normal, tangent,
gradients, material origin and transform; `IMaterial::evalSurface` for materials whose channels share their work.

## Acceptance criteria

- **terrain_materials.AC1** — 2D arrays and RGBA32F tables survive KTX2 (a one-layer array stays an array); layers of
  different shapes are refused; the MeshStore checks a runtime texture's levels.
- **terrain_materials.AC2** — Without scans every layer is procedural (256x256 RGBA8, full mips), a missing folder is
  not an error, tiles divide 16 m, and a library config resolves its paths and fallbacks.
- **terrain_materials.AC3** — Slot weights follow the rules (flat grass, cliffs by slope, snow by temperature, soil and
  rock by depth), sum to 1, and on the slice planet are the committed bits on every platform; depth is non-zero only in
  chunks an edit touches.
- **terrain_materials.AC4** — The toolbox gives the slice's habitable planet its scans' colors and its airless moon a
  palette by default, and either on request; an unknown color mode is `universe.settings_invalid`.
- **terrain_materials.AC5** — Golden `terrain_materials`: hills, cliffs, sand, snow and a dug hole with soil walls,
  over the procedural layers (no local assets); on DX12 (WARP) the scene renders without an error.
- **terrain_materials.AC6** — The slice draws its planets with terrain materials at s = 0.01 and s = 1, from orbit to
  the dug hole, and with `surface_colors = "palette"` (screenshots and GPU times in
  [the findings](../../research/spikes/terrain-materials/README.md)).

## Out of scope

- More than six layers on a body, or per-region layer sets (a per-chunk palette of slots): VCH-146.
- Parallax or displacement from the height maps, cliff-specific projection, coarse borders from orbit: VCH-147.
- Vegetation, rocks and other scattered meshes on the layers (the renderer pass's vegetation items).
- A real climate model (the toolbox's fields are a stand-in until the climate research's stage).
- Water and wet shading.

## Open questions

None.

## Links

- ADRs: [0098](../../adr/0098-terrain-materials.md), [0093](../../adr/0093-gpu-driven-core.md),
  [0090](../../adr/0090-rhi-texture-kinds-updates-and-raster-state.md),
  [0086](../../adr/0086-amendments-from-building-voxel.md)
- Backlog: VCH-130 (done), VCH-146, VCH-147
- Guides: [terrain materials](../../guides/terrain-materials.md), [voxel terrain](../../guides/voxel-terrain.md),
  [space realization](../../guides/space-realization.md)
