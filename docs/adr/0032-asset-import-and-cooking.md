# 0032: glTF import and cooked runtime formats

Date: 2026-11-25

## Status

Accepted

## Context

The first real scenes come from glTF 2.0, the format the engine's conventions already match
([ADR 0010](0010-conventions-and-math.md)). Source files are not what a renderer wants. Their PNG and JPEG
textures must be decoded, mipmapped and compressed. Their accessors come in every layout. Their materials are
JSON. [ADR 0007](0007-asset-formats-and-identity.md) gives every source asset a stable id in a `.meta` sidecar;
the pieces inside a glTF need stable ids too. Hot reload ([spec](../specs/hot-reload/spec.md)) already swaps
assets behind handles.

## Decision

**Import with fastgltf** (v1.0.1, MIT). It is fast (simdjson), validates the file, and resolves buffers and
accessors. Its own CMake pins simdjson 5.1.0 and simdutf 9.3.3 by SHA256. Images are decoded with `stb_image`,
already in the tree.

**Cook into runtime formats**, all defined in `engine/asset`:

| Asset | Runtime form | On disk (cache) |
|---|---|---|
| `MeshAsset` | 48-byte vertices `{position f32x3, normal f32x3, tangent f32x4, uv f32x2}`, `uint32` indices, a `uint32` material slot per triangle, bounds | `.vmesh`: a little-endian header and the three arrays |
| `TextureAsset` | BC7 (`bc7_srgb` for base color and emissive, `bc7_unorm` for normal and metallic-roughness), full mip chain, box-filtered in linear light | KTX2 (no supercompression, valid DFD), written and read by our own ~200-line codec |
| `MaterialAsset` | glTF metallic-roughness + emissive strength, alpha mode and cutoff, double-sided, texture handles, material kind name | JSON |
| `SceneAsset` | node hierarchy (name, TRS, mesh, children), meshes' material slots, lights, the handles of every sub-asset | JSON |

- **Missing data is filled in:** normals become flat normals, missing tangents come from UVs (per-triangle
  basis, accumulated per vertex), and missing UVs are zero. Indices are generated for unindexed primitives. All
  primitives of a glTF mesh become one `MeshAsset`, with one material slot per primitive.
- **BC7 with bc7enc** (richgel999/bc7enc_rdo, MIT). The fast perceptual mode is used for sRGB textures,
  linear for the rest. **`max_texture_size`** in the source's `.meta` (default 0 = no limit) downsizes on import.
- **The cache** lives in `<project>/.veach/cache/<source id>/`, keyed by a hash of the source bytes, the
  referenced files, the import settings and the cooker version. Cooking is deterministic, so a cache hit and a
  fresh cook give identical data. The cache is never committed (`.veach/` is ignored).
- **Sub-asset ids** are derived, not stored: `id = uuid_v5(source id, "mesh/3" | "material/0" | "texture/5")`.
  They stay stable as long as the glTF keeps its order, which re-exporting from the same tool does. Sub-assets
  are registered in the `AssetServer` under `path#mesh/3` and loaded through the same typed `Handle<T>`.
- **Loading a `.gltf` returns `Handle<SceneAsset>`.** The sub-assets load with it, synchronously, as every v0
  load does. Reloading the source re-cooks it and swaps every sub-asset in place
  ([hot reload](../specs/hot-reload/spec.md)).
- **Instancing:** a `SceneInstance { path }` component (Scriptable) spawns one child entity per node, with
  `Transform`, `MeshInstance { mesh, materials }` (handles, not Scriptable) and light components. Removing the
  component despawns them.

## Consequences

- Renderers get exactly one vertex layout and one compressed texture format, on both backends: Apple-silicon
  Metal and WARP support BC7.
- The first import of a large model is slow, because BC7 encoding dominates. The cache and `max_texture_size`
  keep CI and iteration fast.
- Re-ordering a glTF changes its sub-asset ids. When references to sub-assets start being saved (scenes,
  prefabs), the `.meta` will record a name → id map.
- glTF features outside the list (skins, morph targets, animation, cameras, KHR material extensions other
  than emissive strength, Basis-compressed KTX2 sources) are ignored with a warning, not an error.

## Alternatives considered

- **tinygltf or cgltf:** both fine, but fastgltf is faster, actively maintained and C++-native, with accessor
  tools that cut our code.
- **libktx for KTX2:** complete, but a heavy build (with Basis Universal) for a container we only write and
  read in one shape.
- **Uncompressed RGBA8 textures:** simpler, but 4× the memory, and it would hide format bugs until later.
- **Cooking in a separate offline tool:** the plan once assets get large. For now import-on-load plus a cache
  keeps one path.
