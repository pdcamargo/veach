# 0098: Terrain materials: six slot weights per vertex from the generator's rules, layers blended per pixel in body space, palettes opt-in

Date: 2026-11-28

## Status

Accepted. Builds on [0093](0093-gpu-driven-core.md) (binned shading), [0090](0090-rhi-texture-kinds-updates-and-raster-state.md)
(texture arrays, terrain anisotropy) and [0086](0086-amendments-from-building-voxel.md) (chunk meshes on their jobs).
VCH-130, [spec](../specs/terrain-materials/spec.md).

## Context

Terrain chunks were runtime meshes with standard materials chosen per triangle by the voxel material id (lowland,
highland, rock): flat colors, blocky borders from orbit, the same green in a dug hole as in the meadow. The renderer
pass's item 7 asks for scanned PBR layers placed by the generator, blended naturally, without visible tiling or
seams, coherent from orbit to the ground, with an opt-in procedural color. Constraints:

- The vertex is the engine's 48-byte `MeshVertex` (position, normal, tangent, uv), shared by cooked meshes, BLAS
  builds and both renderers. A terrain-only format would fork all of them.
- Materials see a `MaterialContext` (surface and record), not the instance or the triangle; the material record has
  four 2D texture handles, and on Metal a handle is typed: a 2D handle cannot be read as an array.
- Chunk-local positions are precise; body-frame positions at s = 1 (6.4e6 m) are not, in float. Chunk origins are
  multiples of 16 m at every level. Near the ground render space is the body frame; above the atmosphere it is not.
- Material ids per vertex cannot be interpolated across a triangle; the voxel layer's per-sample material is one u8.
- The scans (Quixel Megascans from Fab) cannot be in the repository; CI must not need them.

## Decision

1. **Weights per vertex, rules on the CPU.** A body's surface has at most six slots; each chunk vertex carries the six
   slot weights in its tangent (0-3) and uv (4-5), normalized, computed on the chunk's job by `TerrainSurface` rules:
   products of smooth windows over altitude, slope (from the vertex normal and the body's up), depth below the
   generated surface and two game fields, the largest of a slot's rules winning. Weights interpolate correctly
   across triangles (ids would not), the vertex format is unchanged, and the weights are canonical (+ - * / sqrt over
   lattice positions and canonical fields), so they are hashed in tests and match on every platform.
2. **Depth only where edits are.** A chunk an edit touches asks its density source for the generated distance at
   each vertex (`DensitySource::generated_distance`); elsewhere the mesh is the generated surface and depth is 0. A
   dig's walls show soil, then rock, at no cost to untouched terrain.
3. **The material blends per pixel in object space.** `TerrainMaterial` (a material kind apps opt in to with
   `veach_add_terrain_material`) projects triplanar in the chunk's object space, which is the body frame translated:
   tiles of 16 / n m repeat exactly across chunk origins, and the projection is attached to the planet whatever render
   space is. `SurfaceInput` gains object-space position, normal, tangent (as stored), pixel gradients, the instance's
   linear transform and the draw's `material_origin` (the chunk origin in the body frame modulo 65,536 m) for a macro
   noise that is continuous across chunks. Each layer is sampled at a near and a far tile, the near one varied by the
   far one's departure from the mean, and faded by the pixel footprint relative to each tile to the layer's mean
   color and roughness: beyond that no texture is read, and the planet from orbit shows the colors of its ground.
   Height blending sharpens borders where textures are seen and leaves the weights as they are far away, where the
   heights are only means (otherwise borders follow coarse chunks' triangles). `IMaterial::evalSurface` lets the
   material compute color, normal and roughness from one set of fetches.
4. **Two arrays and a table per surface, through the material record.** Albedo with height in alpha (BC7 sRGB), and
   normal xy, roughness and AO (BC7 unorm), one array layer per library layer; the record's `custom1` became two typed
   `Texture2DArray` handles (`MaterialAsset::array_textures`) and the per-slot table is an RGBA32F texture in the normal
   slot. Runtime textures live in the `MeshStore`. Materials may ask for anisotropic filtering
   (`MaterialAsset::max_anisotropy`; terrain uses `rhi::terrain_anisotropy`).
5. **Libraries cook scans once and stand in for them.** A game lists layers by role name with a scan folder; the
   cooker finds the maps by their names' endings, checks the normal map's green against the height's slopes, builds
   Toksvig-widened roughness mips, encodes BC7 on the job system, and caches KTX2 keyed by the sources' names, sizes
   and times. A missing scan is a procedural layer (tileable noise, its normal, a fallback color), and a library with
   no scans is small RGBA8, so CI, tests and goldens never depend on local assets.
6. **Colors are the toolbox's choice, overridable.** `space::surface_desc` picks a biome set (temperate, arid, cold,
   airless) from composition and climate, its climate fields from a procgen program in planet meters plus latitude
   and lapse, and its colors: the scans' on potentially habitable planets, palettes (from the star's temperature and
   the body's address) elsewhere. `Universe.surface_colors` forces "scanned" or "palette". A palette layer is its
   palette color times the scan's luminance over its mean: the scan's light and shade, the planet's color.
7. **The instance record grows to 256 bytes** for the material origin (temporal AA took its last padding).

## Consequences

- The slice's planet reads as a planet from orbit (snow on peaks and poles, green lowlands, sand where it is dry) and
  as scanned ground on landing; the hole shows soil walls. Screenshots and GPU times:
  [findings](../research/spikes/terrain-materials/README.md).
- Six layers per body. A planet with more biomes than six layers can show needs per-region slot sets (VCH-146).
- Terrain chunks' tangents and uvs are no longer tangents and uvs; a standard material on them would read the
  weights. TerrainLook without a surface keeps the old behavior.
- The toolbox's climate is a stand-in (an equilibrium temperature plus Earth's greenhouse on temperate worlds); the
  climate research's stage replaces it without changing the engine side.
- Every material record and the path tracer's surfaces carry the new SurfaceInput fields; compilers drop the unused
  ones (no measurable change in the non-terrain goldens).

## Alternatives considered

- **Material ids plus barycentric weights** (the triangle's three ids, blended by barycentrics): needs the triangle's
  vertices in the material and loses blends wider than one triangle; ids at coarse LODs alias into blocks.
- **Rules evaluated per pixel from interpolated climate fields:** fewer CPU cycles, any number of layers, but every
  pixel loads and evaluates every slot's rules (tens of table reads), and the weights are not canonical.
- **A terrain vertex format:** forks cooked meshes, BLAS builds and both renderers for one material.
- **Triplanar in render space:** swims as the planet turns under an inertial camera, and its coordinates would need
  the render origin's 64 m grid to divide every tile.
- **Splat maps per chunk** (a texture of weights): resolution tied to texels, not to the mesh; another resource per
  chunk to stream and retire.
