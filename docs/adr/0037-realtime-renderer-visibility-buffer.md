# 0037: The real-time renderer: a visibility buffer over cook-time meshlets, shaded with the shared materials

Date: 2026-11-25

## Status

Accepted.

## Context

Veach has a reference path tracer ([ADR 0034](0034-lights-and-reference-path-tracer.md)) and no fast path. The
real-time renderer is not a preview: it is the renderer games ship with, and later systems build on it:
Tessera (virtualized geometry) takes over its geometry, Candela (GI) replaces its ambient term. Three
constraints shape it:

- **One material definition, two renderers.** A material is a Slang type implementing `IMaterial`
  ([ADR 0033](0033-material-model.md)). It must render identically in the path tracer and here, with no second
  implementation to drift.
- **Tessera later, without a format change.** Geometry must already be in clusters, in the layout Tessera's
  cluster hierarchy will extend.
- **Both backends, verified on WARP.** No feature that WARP or Apple GPUs lack (mesh shaders are optional on
  both, so v0 does not depend on them).

## Decision

**Cook-time meshlets.** The glTF cooker ([ADR 0032](0032-asset-import-and-cooking.md)) splits every primitive
into meshlets with meshoptimizer (`meshopt_buildMeshlets`, at most 64 vertices and 124 triangles, one material
slot each) and stores them in the cooked mesh:

| Array | Element | Use |
|---|---|---|
| `meshlets` | `{vertex_offset, triangle_offset, vertex_count, triangle_count, slot, lod, parent, flags}` | one per meshlet |
| `meshlet_vertices` | `u32`, an index into the mesh's vertices | per meshlet vertex |
| `meshlet_triangles` | 3 × `u8`, local vertex indices, padded to 4 bytes | per meshlet triangle |
| `meshlet_bounds` | sphere (center, radius), normal cone (axis, cutoff), `error` | culling, LOD |

`lod`, `parent`, `error` and `flags` are 0 in v0 (every meshlet is level 0, a root). They are the fields
Tessera's cluster hierarchy fills: its simplified levels append more meshlets to the same arrays. The ray
tracing BLAS keeps using the plain index buffer, which the cooker also stores.

**Visibility buffer.** One raster pass draws every visible instance's meshlets with vertex pulling: a draw per
instance of `124 × 3` vertices and one instance per meshlet. The vertex shader reads its meshlet's triangle and
vertices from buffers (no fixed-function vertex input) and collapses triangles past `triangle_count`. Each
pixel stores `{instance, meshlet << 7 | triangle}` in an `rg32_uint` target, with reverse-Z depth. Alpha-masked
materials are tested in the fragment shader with the shared `evaluateAlpha`. This is the classic mesh-shader
fallback; a mesh-shader path can replace the vertex stage later without changing the buffer.

**Shading in one compute pass.** For each pixel the shading pass reads the visibility buffer, rebuilds the
triangle, and intersects the pixel's camera ray with it for exact barycentrics; the rays through the neighboring
pixels give the UV derivatives for texture filtering. It then calls the generated `evaluateSurface` and the BRDF
the path tracer uses. The surface reconstruction, the BRDF, light sampling and shadow rays move out of
`path_tracer.slang` into shared modules (`veach/scene.slang`, `veach/brdf.slang`, `veach/lights.slang`), so the
two renderers cannot drift: the only difference allowed is which integral they estimate.

**Direct lighting** from every light type:

- Punctual lights (directional, point, spot): evaluated exactly, each with one shadow ray (a ray query against
  the same TLAS as the path tracer; hard shadows).
- Emissive surfaces: `emissive_samples` (default 4) light samples per pixel from the path tracer's emissive
  triangle distribution, each with a shadow ray. Unbiased and noisy until the denoiser (VCH-022).
- Emission seen directly.

**Ambient: a stopgap.** Until Candela exists, shadowed areas get `sky radiance × diffuse albedo × AO`, where AO
is the fraction of `ao_rays` (default 4) cosine-distributed rays that travel `ao_radius` (default 1 m) without
a hit. It is labeled as a stopgap everywhere: Candela replaces it, keeping at most a short-range AO for detail
Candela misses. The path tracer never applies AO (it would count occlusion twice); it can only show AO as a
debug view.

**HDR, then one display transform.** Both renderers write linear radiance (nits) to an `rgba16_float` target.
One shared pass applies exposure and the tonemapper ([ADR 0038](0038-exposure-and-tonemapping.md)) into the
sRGB `scene_color` that screenshots and presentation read.

**Which renderer draws a view** follows its camera: a `PathTracer` component selects the reference; otherwise
the real-time renderer draws it.

## Consequences

- A material is written once and is correct in both renderers by construction; a test renders `GlowMaterial`
  in both and compares them.
- The path tracer is ground truth for the real-time renderer: a direct-lighting-only mode of the path tracer
  converges to what the real-time renderer estimates, so the difference is measurable per scene (FLIP).
- Shading cost scales with pixels, not with overdraw: one material evaluation per pixel.
- Vertex pulling with one instance per meshlet wastes vertex invocations on partly filled meshlets. That is the
  price of not requiring mesh shaders, and Tessera's culling pass will replace it.
- Analytic derivatives instead of quad derivatives: texture filtering matches across both renderers' code
  paths (the path tracer passes zero derivatives and samples mip 0).
- Shadows are hard and emissive lighting is noisy until the soft-shadow and denoiser milestone.

## Alternatives considered

- **Forward or deferred shading with a G-buffer:** a second material path per pass (or fat G-buffers), and
  overdraw shading cost. The visibility buffer keeps one material evaluation per pixel and the same code as
  the path tracer.
- **Mesh shaders now:** the best fit for meshlets, but optional on our targets and on WARP. The buffer format
  does not depend on how meshlets are rasterized, so they can come later.
- **Shadow maps:** a second visibility representation to tune per light. Ray queries are already required
  (ADR 0002) and give exact hard shadows with the same TLAS as the path tracer.
- **Screen-space AO:** cheap, but view-dependent and wrong at screen edges; ray-traced AO matches what Candela
  will compute and needs no extra buffers.
