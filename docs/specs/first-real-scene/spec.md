# First real scene

**Status:** Implemented
**Area:** render, asset
**Owner:** dev lead

## Summary

Veach loads real glTF scenes and renders them physically, as ground truth. glTF files are imported with
fastgltf and cooked into Veach's runtime formats: meshes, KTX2/BC7 textures, materials, and scenes with stable
ids and typed handles. Materials are Slang interfaces whose functions any material can override one at a time.
Directional, point and spot lights and emissive surfaces light the scene. A progressive reference path tracer
renders it, and goldens of a Cornell box and a textured Khronos model gate Metal and DX12 (WARP).

## Motivation

The triangle was first light. Candela (real-time GI) needs a reference to be measured against, and every later
renderer needs the asset pipeline, the material model and the lights. Roadmap: VISION items 4 (content) and 5
(the renderer).

## Requirements

**Assets** ([ADR 0032](../../adr/0032-asset-import-and-cooking.md))
- MUST import `.gltf` and `.glb` (fastgltf): meshes, materials (metallic-roughness, emissive strength, alpha
  modes, double-sided), base color, metallic-roughness, normal and emissive textures, the node hierarchy, and
  `KHR_lights_punctual` lights.
- MUST cook them into runtime formats: `MeshAsset` (48-byte vertices, `uint32` indices, a material slot per
  triangle), `TextureAsset` (KTX2, BC7, sRGB or linear by use, full mip chain), `MaterialAsset` and
  `SceneAsset`. Cooked files are cached under `<project>/.veach/cache/` and reused while the source is unchanged.
- Every cooked sub-asset MUST have a stable `AssetId` derived from the source's `.meta` id, and a typed
  `Handle<T>`.
- A `SceneInstance { path }` component MUST spawn the scene as child entities: `Transform`, `MeshInstance`,
  lights.

**Materials** ([ADR 0033](../../adr/0033-material-model.md))
- MUST be Slang types conforming to `IMaterial`, whose functions (`evalBaseColor`, `evalNormal`,
  `evalMetallicRoughness`, `evalEmission`) have default implementations. A material overrides only what it
  changes (`override`). Every renderer evaluates surfaces through `evaluateSurface(kind, context)`.
- Apps register material kinds with `veach_add_material()`. A glTF material picks one by name
  (`extras.veach_material`), and an unknown name is an error with a suggestion.

**Lights and the reference path tracer** ([ADR 0034](../../adr/0034-lights-and-reference-path-tracer.md))
- `DirectionalLight` (lux), `PointLight` and `SpotLight` (candela): reflected, Scriptable, and looking down −Z.
  Emissive surfaces emit `evalEmission` (nits).
- The path tracer MUST be progressive and unbiased: GGX and Lambert BSDF, next-event estimation to punctual
  lights and emissive triangles, MIS, Russian roulette, and deterministic per-pixel RNG. It accumulates while
  nothing changes and restarts when the view, the scene or its settings do.
- `PathTracer` on the camera entity enables it (samples per frame, max bounces, sky radiance). `Camera.ev100`
  sets exposure.
- MCP `path_trace { samples, path? }` MUST accumulate exactly `samples` samples from scratch and return the PNG.

## API sketch

```lua
local veach = require("@veach")
veach.spawn("box"):set(veach.components.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })
local camera = veach.spawn("camera")
camera:set(veach.components.Transform, { translation = vector.create(0, 1, 3.4), rotation = veach.quat.identity(), scale = vector.one })
camera:set(veach.components.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
camera:set(veach.components.PathTracer, { samples_per_frame = 16, max_bounces = 8, sky = vector.zero })
```

```slang
import veach.material;
struct GlowMaterial : IMaterial {
    override float3 evalEmission(MaterialContext c) { return stripes(c.surface.uv) * c.record.emission_strength; }
};
```

## Acceptance criteria

- **scene.AC1**: Importing `cornell_box.gltf` produces a `SceneAsset`, its meshes, materials and textures with
  stable ids (identical across two imports and after deleting the cache). A second import reuses the cache. A
  KTX2 written by the cooker reads back with the same format, size, mips and blocks.
- **scene.AC2**: A BC7 cook of a test image decodes within PSNR ≥ 35 dB of the source.
- **scene.AC3**: `SceneInstance` spawns one entity per glTF node, with the glTF hierarchy, transforms, mesh
  instances and lights.
- **scene.AC4**: A glTF material naming `GlowMaterial` uses the lab's material, which overrides only
  `evalEmission`. A misspelled name fails with a suggestion.
- **scene.AC5**: `golden: cornell box (path traced)` matches `tests/golden/cornell_box.png` on Metal and DX12/WARP.
  The scene has an area light (an emissive quad), a point light, the glow block, and an alpha-masked panel with
  a BC7 checker texture. Reruns on one backend are bit-identical.
- **scene.AC6**: `golden: flight helmet (path traced)` matches `tests/golden/flight_helmet.png` on Metal and
  DX12/WARP: textured and normal-mapped, under a directional and a spot light.
- **scene.AC7**: Over MCP, `path_trace { samples = 64 }` returns the same image as the golden run of the same
  scene (FLIP).
- **scene.AC8**: The accumulation restarts when the camera moves, and converges: 4× the samples lowers the
  error against a high-sample reference.
- **scene.AC9**: RGBA8 and BC7 textures with full mip chains upload, and mip 0 reads back byte for byte, on Metal
  and DX12. BC7 sizes are multiples of 4 on every backend (D3D12 cannot upload an unaligned top level); the
  cooker rounds them up and the RHI rejects others with `rhi.invalid_texture`.

## Out of scope

- Raster scene rendering, Tessera and Candela; skinning and animation; KHR material extensions beyond emissive
  strength; texture streaming; glTF cameras; KTX2 Basis supercompression; importer settings beyond
  `max_texture_size`.

## Open questions

- Sponza: its files are under the CRYENGINE Limited License Agreement, which needs the owner's acceptance.
  FlightHelmet (CC0) stands in until then (VCH-055).

## Links

- ADRs: [0032](../../adr/0032-asset-import-and-cooking.md), [0033](../../adr/0033-material-model.md),
  [0034](../../adr/0034-lights-and-reference-path-tracer.md), [0028](../../adr/0028-golden-images-across-backends.md)
- Specs: [hot-reload](../hot-reload/spec.md), [render-views](../render-views/spec.md), [transforms](../transforms/spec.md)
- Backlog: VCH-051 (glTF import), VCH-052 (materials v0), VCH-053 (lights), VCH-054 (reference path tracer)
