# 0035: Material model: Slang interfaces with overridable defaults

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0033).

## Context

Every renderer Veach has must ask the same question at a surface point: what does this surface look like? Today
that is the real-time raster renderer ([ADR 0039](0039-realtime-renderer-is-raster-pbr.md)) and photo mode
([ADR 0041](0041-photo-mode.md)). If each read material parameters its own way, materials would drift and photo
mode would stop being ground truth. Users must also customize a material without forking the standard one (only the
emission, only the normal): Bevy's `ExtendedMaterial`, without its restriction to one render path.

Slang facts (pinned slangc 2026.22): interfaces may carry **default implementations** that conforming types replace
with `override`, compiling to DXIL and Metal; struct inheritance works but warns E30816 ("unstable and will be
removed").

## Decision

**A material is a Slang type conforming to `IMaterial`** (`engine/render/shaders/veach/material.slang`). Its four
functions describe a surface, each with a default:

```slang
interface IMaterial {
    float4 evalBaseColor(MaterialContext c)         { return standardBaseColor(c); }         // rgb, alpha
    float3 evalNormal(MaterialContext c)            { return standardNormal(c); }            // world space, shading
    float2 evalMetallicRoughness(MaterialContext c) { return standardMetallicRoughness(c); }
    float3 evalEmission(MaterialContext c)          { return standardEmission(c); }          // radiance, nits
};
struct StandardMaterial : IMaterial {};   // glTF metallic-roughness: every default
struct GlowMaterial : IMaterial {         // the lab's app material: overrides one function
    override float3 evalEmission(MaterialContext c) { return standardEmission(c) * stripes(c.surface.uv); }
};
```

- **The `standard*` functions are public**, so an override can extend the default instead of replacing it.
- **`MaterialContext`** = `SurfaceInput` (world position, geometric and interpolated normals, tangent, UV and its
  derivatives, view direction, front-facing) plus the material's `MaterialRecord`.
- **`MaterialRecord`** is a fixed 128-byte record in one `StructuredBuffer` per scene: the glTF factors, texture and
  sampler `DescriptorHandle`s, alpha mode and cutoff, the material kind, flags, and `float4 custom[2]` for app
  materials. Mirrored in C++ as `render::MaterialRecord` with `static_assert`s on its offsets.
- **Materials describe surfaces; renderers own light transport.** No material function sees lights, rays or the
  BSDF. Each renderer turns `SurfaceSample` (base color, alpha, normal, metallic, roughness, emission) into light.
  This boundary lets one material run unchanged in the raster renderer and in photo mode.
- **Material functions are pure:** no writes, no quad derivatives. They run in compute and ray-query shaders, so
  texture sampling takes an explicit LOD or gradients. They must be deterministic.
- **Kinds and dispatch.** `veach_add_material(<target> <Name> <file.slang> [EMISSIVE])` registers a kind; kind 0 is
  always `StandardMaterial`, app kinds follow in call order. `veach_add_scene_shaders(<target> NAMESPACE <ns>)`
  generates `veach/materials_generated.slang` with `evaluateSurface(kind, c)` (a static `switch` over kinds) and
  `evaluateAlpha(kind, c)` (calls only `evalBaseColor`), plus a C++ table of kind names and `EMISSIVE` flags that
  turns `extras.veach_material` names into kinds. **Every renderer calls these generated functions**; none reads
  `MaterialRecord` fields directly. Shaders that need the dispatch are compiled into each app's shader library
  together with that app's materials.
- **`EMISSIVE`** marks a kind whose `evalEmission` may be non-zero where the record's emissive factor is zero, so
  photo mode samples its triangles as lights. Standard materials are emissive when their factor is.

## Consequences

- One definition of "what a surface looks like" for every renderer, and an extension point that is a single
  function.
- A static switch is portable and inlines per call site. Divergence across many kinds in a wave is a performance
  concern (sort by kind), not a correctness one.
- The record is fixed-size. A material that needs more than `custom` waits for a material-data-buffer extension
  (an ADR when someone needs it).
- New `IMaterial` functions get defaults, so existing materials keep compiling. Procedural and stylized looks
  (research phase) extend this interface or add a parallel one by ADR.

## Alternatives considered

- **Struct inheritance (`struct Glow : StandardMaterial`):** unstable in Slang and slated for removal.
- **Slang dynamic dispatch (existential interface values):** needs `[anyValueSize]` and per-target support, for no
  gain while kinds are known at build time.
- **An uber-shader with feature flags:** every customization becomes a new flag in engine code.
- **Material graphs (node editor):** the long-term authoring tool, but it needs a code target. This interface is
  that target.
