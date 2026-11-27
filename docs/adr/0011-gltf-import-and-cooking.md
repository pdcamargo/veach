# 0011: glTF import and cooked runtime formats

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0032, with the cook-time meshlets of old ADR 0037).

## Context

Scenes come from glTF 2.0, which the engine's conventions already match ([0013](0013-conventions-and-math.md)).
Source files are not what a renderer wants: textures must be decoded, mipmapped and compressed, accessors come in
every layout, materials are JSON. The pieces inside a glTF need stable ids too ([0010](0010-asset-formats-and-identity.md)),
and hot reload ([spec](../specs/hot-reload/spec.md)) swaps assets behind handles.

## Decision

**Import with fastgltf** (v1.0.1, MIT; its CMake pins simdjson 5.1.0 and simdutf 9.3.3 by SHA256). Images are decoded
with `stb_image`.

**Cook into runtime formats**, defined in `engine/asset`:

| Asset | Runtime form | On disk (cache) |
|---|---|---|
| `MeshAsset` | 48-byte vertices `{position f32x3, normal f32x3, tangent f32x4, uv f32x2}`, `uint32` indices, a `uint32` material slot per triangle, bounds, meshlets (below) | `.vmesh`: a little-endian header and the arrays |
| `TextureAsset` | BC7 (`bc7_srgb` for base color and emissive, `bc7_unorm` for normal and metallic-roughness), full mip chain, box-filtered in linear light | KTX2 (no supercompression, valid DFD), our own small codec |
| `MaterialAsset` | glTF metallic-roughness + emissive strength, alpha mode and cutoff, double-sided, texture handles, material kind name | JSON |
| `SceneAsset` | node hierarchy (name, TRS, mesh, children), meshes' material slots, lights, every sub-asset's handle | JSON |

- **Missing data is filled in:** flat normals, tangents from UVs (per-triangle basis accumulated per vertex), zero
  UVs, generated indices for unindexed primitives. All primitives of a glTF mesh become one `MeshAsset`, one material
  slot per primitive.
- **BC7 with bc7enc** (richgel999/bc7enc_rdo, MIT): perceptual mode for sRGB textures, linear for the rest.
  `max_texture_size` in the source's `.meta` (0 = no limit) downsizes on import.
- **Cook-time meshlets.** Every primitive is split with meshoptimizer (`meshopt_buildMeshlets`, at most
  `meshlet_max_vertices` = 64 vertices and `meshlet_max_triangles` = 124 triangles, one material slot each):

  | Array | Element | Use |
  |---|---|---|
  | `meshlets` | `Meshlet {vertex_offset, triangle_offset, vertex_count, triangle_count, slot, lod, parent, flags}`, 32 bytes | one per meshlet |
  | `meshlet_vertices` | `u32`, an index into the mesh's vertices | per meshlet vertex |
  | `meshlet_triangles` | 3 × `u8` local vertex indices, padded to 4 bytes | per meshlet triangle |
  | `meshlet_bounds` | `MeshletBounds {sphere, cone, error}`, 48 bytes | culling, LOD |

  `lod`, `parent`, `error` and `flags` are reserved for a cluster hierarchy: today every meshlet is level 0 with no
  parent (`meshlet_no_parent`), error 0 and flags 0. The plain index buffer is kept too (acceleration structures for
  photo mode build from it).
- **The cache** lives in `<project>/.veach/cache/<source id>/`, keyed by a hash of the source bytes, referenced files,
  import settings and cooker version. Cooking is deterministic: a cache hit and a fresh cook give identical data.
  `.veach/` is never committed.
- **Sub-asset ids** are derived, not stored: `id = uuid_v5(source id, "mesh/3" | "material/0" | "texture/5")`, stable
  while the glTF keeps its order. Sub-assets are registered under `path#mesh/3` and loaded through the same typed
  `Handle<T>`.
- **Loading a `.gltf` returns `Handle<SceneAsset>`**; sub-assets load with it. Reloading the source re-cooks it and
  swaps every sub-asset in place.
- **Instancing:** a `SceneInstance { path }` component (Scriptable) spawns one child entity per node, with
  `Transform`, `MeshInstance` (not Scriptable) and light components. Removing the component despawns them.

## Consequences

- Renderers get one vertex layout, one meshlet layout and one compressed texture format on both backends (Apple GPUs
  and WARP support BC7).
- First import of a large model is slow (BC7 encoding dominates); the cache and `max_texture_size` keep CI and
  iteration fast.
- Re-ordering a glTF changes its sub-asset ids. Once scenes or prefabs save sub-asset references, the `.meta` will
  record a name → id map.
- glTF features outside this list (skins, morph targets, animation, cameras, KHR material extensions other than
  emissive strength, Basis-compressed KTX2) are ignored with a warning.
- Procedural geometry (terrain, vegetation) will not come through this path; how it is represented is a research-phase
  question.

## Alternatives considered

- **tinygltf or cgltf:** fine, but fastgltf is faster, maintained and C++-native.
- **libktx:** complete, but a heavy build (with Basis Universal) for a container we only write and read in one shape.
- **Uncompressed RGBA8 textures:** 4× the memory, and it would hide format bugs.
- **Meshlets built at load time, not cooked:** repeated work every run; cooking caches it.
- **A separate offline cooking tool:** the plan once assets get large; import-on-load plus a cache keeps one path for now.
