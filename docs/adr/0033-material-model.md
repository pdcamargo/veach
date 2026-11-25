# 0033: Material model: Slang interfaces with overridable defaults

Date: 2026-11-25

## Status

Accepted

## Context

Every renderer Veach will have needs to ask the same question at a surface point: what does this surface look
like? That covers the reference path tracer now, and raster, Candela (GI) and reflections later. If each
renderer reads material parameters its own way, materials drift between them, and the path tracer stops being a
reference for anything. Users must also be able to customize a material without forking the standard one:
change only the emission, or only the normal. That is Bevy's `ExtendedMaterial`, without its restriction to one
render path.

Two Slang facts, checked with our pinned slangc 2026.22:
- Interfaces may carry **default implementations**, and conforming types replace one with `override`. This
  compiles to HLSL/DXIL and Metal.
- Struct inheritance works but warns E30816: "support for inheritance is unstable and will be removed".

## Decision

**A material is a Slang type conforming to `IMaterial`** (`engine/render/shaders/veach/material.slang`). Its
four functions describe a surface, and each has a default:

```slang
interface IMaterial {
    float4 evalBaseColor(MaterialContext c)         { return standardBaseColor(c); }         // rgb, alpha
    float3 evalNormal(MaterialContext c)            { return standardNormal(c); }            // world space, shading
    float2 evalMetallicRoughness(MaterialContext c) { return standardMetallicRoughness(c); }
    float3 evalEmission(MaterialContext c)          { return standardEmission(c); }          // radiance, nits
};
struct StandardMaterial : IMaterial {};   // glTF metallic-roughness: every default
struct GlowMaterial : IMaterial {         // an app material: overrides one function
    override float3 evalEmission(MaterialContext c) { return standardEmission(c) * stripes(c.surface.uv); }
};
```

- **The `standard*` functions are public**, so an override can extend the default instead of replacing it.
- **`MaterialContext`** = `SurfaceInput` (world position, geometric and interpolated normals, tangent, UV, view
  direction, front-facing) plus the material's `MaterialRecord`.
- **`MaterialRecord`** is a fixed 128-byte record in one `StructuredBuffer` per scene. It holds the glTF factors,
  the texture and sampler `DescriptorHandle`s, the alpha mode and cutoff, the material kind, and `float4
  custom[2]` for app materials. It is mirrored in C++ as `render::MaterialRecord`, with the VCH-041 layout check.
- **Materials describe surfaces; renderers own light transport.** No material function sees lights, rays or
  the BSDF. Every renderer turns `SurfaceSample` (base color, alpha, normal, metallic, roughness, emission)
  into light with the same glTF BRDF. This boundary is what lets one material run unchanged in the path tracer,
  raster and Candela.
- **Material functions are pure:** no writes, no derivatives. They run in compute and ray-query shaders, so
  texture sampling takes an explicit LOD (`sampleTexture(handle, uv, lod)`). They must be deterministic.
- **Kinds and dispatch.** `veach_add_material(<target> <Name> <file.slang> [EMISSIVE])` registers a kind. Kind 0
  is always `StandardMaterial`, and app kinds follow in call order. The build generates
  `veach/materials.generated.slang`:

  ```slang
  SurfaceSample evaluateSurface(uint kind, MaterialContext c) {
      switch (kind) { case 1: return evaluate(GlowMaterial(), c); default: return evaluate(StandardMaterial(), c); }
  }
  ```

  It also generates a C++ table of kind names and `EMISSIVE` flags, which the renderer uses to turn
  `extras.veach_material` names into kinds and to find light-emitting triangles. **Every renderer calls
  `evaluateSurface`**; none reads `MaterialRecord` fields directly. Renderers whose shaders need the dispatch
  (the path tracer today) are compiled into each app's shader library, together with that app's materials.
- **`EMISSIVE`** marks a kind whose `evalEmission` may be non-zero where the record's emissive factor is
  zero. The path tracer then samples its triangles as lights. Standard materials are emissive when their
  factor is.

## Consequences

- One definition of "what a surface looks like" for every renderer, and a material extension point that is a
  single function: the lab's `GlowMaterial` overrides only `evalEmission`.
- A static switch is portable (DXIL and Metal) and inlines per call site. Divergence across many kinds in a
  wavefront is a later performance concern (sort by kind), not a correctness one.
- The record is fixed-size. A material that needs more than its `custom` space waits for a "material data
  buffer" extension (an ADR when someone needs it).
- Changing `IMaterial`'s functions changes every material: new functions get defaults, so existing materials
  keep compiling.

## Alternatives considered

- **Struct inheritance (`struct Glow : StandardMaterial`):** reads like C++, but Slang has marked it unstable
  and slated for removal.
- **Slang dynamic dispatch (existential interface values):** needs `[anyValueSize]` and per-target support, for
  no gain over a generated switch while kinds are known at build time.
- **An uber-shader with feature flags:** works until the first custom material; then every customization is a
  new flag in engine code.
- **Material graphs (node editor):** the long-term authoring tool, but it needs a code target anyway. This
  interface is that target.
