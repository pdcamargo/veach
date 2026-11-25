// The material model (docs/adr/0033-material-model.md). A material is a type
// conforming to IMaterial; every function has a default, and a material
// overrides only what it changes:
//
//   import veach.material;
//   struct GlowMaterial : IMaterial {
//       override float3 evalEmission(MaterialContext c) { return standardEmission(c) * stripes(c.surface.uv); }
//   };
//
// Materials describe surfaces; renderers own light transport. Material
// functions are pure and sample textures at an explicit LOD (no derivatives:
// they run in compute and ray-query shaders).

/// Where on a surface the material is evaluated. World space.
struct SurfaceInput {
    float3 position;
    float3 geometric_normal;  ///< faces the viewer
    float3 normal;            ///< interpolated vertex normal, same side as geometric_normal
    float4 tangent;           ///< xyz, w = bitangent sign
    float2 uv;
    float3 view;              ///< towards the viewer
};

static const uint material_has_base_color_texture = 1u << 0;
static const uint material_has_metallic_roughness_texture = 1u << 1;
static const uint material_has_normal_texture = 1u << 2;
static const uint material_has_emissive_texture = 1u << 3;
static const uint material_alpha_mask = 1u << 4;
static const uint material_alpha_blend = 1u << 5;
static const uint material_double_sided = 1u << 6;
static const uint material_emissive = 1u << 7;  ///< sampled as a light (ADR 0034)

/// 128 bytes, one per material in the scene's material buffer. Mirrors render::MaterialRecord (C++).
/// Only float4, uint2 handles and scalars: identical layout on DXIL and Metal.
struct MaterialRecord {
    uint kind;  ///< which IMaterial type evaluates it (the generated evaluateSurface switch)
    uint flags;
    float alpha_cutoff;
    float normal_scale;
    float4 base_color;
    float4 emission_metallic;  ///< xyz: emissive factor x strength (nits), w: metallic
    float roughness;
    float emissive_weight;  ///< luminance weight for light sampling
    DescriptorHandle<SamplerState> sampler;
    DescriptorHandle<Texture2D<float4>> base_color_texture;          ///< sRGB
    DescriptorHandle<Texture2D<float4>> metallic_roughness_texture;  ///< G roughness, B metallic
    DescriptorHandle<Texture2D<float4>> normal_texture;              ///< tangent space
    DescriptorHandle<Texture2D<float4>> emissive_texture;            ///< sRGB
    float4 custom0;  ///< for app materials
    float4 custom1;
};

struct MaterialContext {
    SurfaceInput surface;
    MaterialRecord record;
};

/// What a material says about a surface point. Renderers turn it into light with the glTF BRDF.
struct SurfaceSample {
    float3 base_color;
    float alpha;
    float3 normal;  ///< shading normal, world space, unit length
    float metallic;
    float roughness;
    float3 emission;  ///< radiance leaving the front side, nits
};

float4 sampleTexture(DescriptorHandle<Texture2D<float4>> texture, DescriptorHandle<SamplerState> sampler, float2 uv) {
    return texture.SampleLevel(sampler, uv, 0.0);
}

// ---- the defaults: glTF metallic-roughness. Public, so overrides can extend them. ------------------------

float4 standardBaseColor(MaterialContext c) {
    float4 color = c.record.base_color;
    if ((c.record.flags & material_has_base_color_texture) != 0) {
        color *= sampleTexture(c.record.base_color_texture, c.record.sampler, c.surface.uv);
    }
    return color;
}

float3 standardNormal(MaterialContext c) {
    const float3 n = normalize(c.surface.normal);
    if ((c.record.flags & material_has_normal_texture) == 0) {
        return n;
    }
    float3 t = c.surface.tangent.xyz - n * dot(n, c.surface.tangent.xyz);
    if (dot(t, t) < 1e-12) {
        return n;
    }
    t = normalize(t);
    const float3 b = cross(n, t) * (c.surface.tangent.w < 0.0 ? -1.0 : 1.0);
    float3 m = sampleTexture(c.record.normal_texture, c.record.sampler, c.surface.uv).xyz * 2.0 - 1.0;
    m.xy *= c.record.normal_scale;
    const float3 shading = normalize(m.x * t + m.y * b + m.z * n);
    // A normal map must not tip the shading normal below the surface the viewer sees.
    return dot(shading, c.surface.geometric_normal) > 0.0 ? shading : n;
}

float2 standardMetallicRoughness(MaterialContext c) {
    float2 mr = float2(c.record.emission_metallic.w, c.record.roughness);
    if ((c.record.flags & material_has_metallic_roughness_texture) != 0) {
        const float4 s = sampleTexture(c.record.metallic_roughness_texture, c.record.sampler, c.surface.uv);
        mr *= float2(s.b, s.g);
    }
    return mr;
}

float3 standardEmission(MaterialContext c) {
    float3 e = c.record.emission_metallic.xyz;
    if ((c.record.flags & material_has_emissive_texture) != 0) {
        e *= sampleTexture(c.record.emissive_texture, c.record.sampler, c.surface.uv).rgb;
    }
    return e;
}

interface IMaterial {
    /// rgb and alpha (coverage for alpha masking and blending).
    float4 evalBaseColor(MaterialContext c) { return standardBaseColor(c); }
    /// World-space shading normal.
    float3 evalNormal(MaterialContext c) { return standardNormal(c); }
    /// x metallic, y perceptual roughness.
    float2 evalMetallicRoughness(MaterialContext c) { return standardMetallicRoughness(c); }
    /// Emitted radiance, nits.
    float3 evalEmission(MaterialContext c) { return standardEmission(c); }
};

/// glTF metallic-roughness: every default.
struct StandardMaterial : IMaterial {};

SurfaceSample evaluate<M : IMaterial>(M material, MaterialContext c) {
    SurfaceSample s;
    const float4 base = material.evalBaseColor(c);
    s.base_color = base.rgb;
    s.alpha = base.a;
    s.normal = material.evalNormal(c);
    const float2 mr = saturate(material.evalMetallicRoughness(c));
    s.metallic = mr.x;
    s.roughness = mr.y;
    s.emission = material.evalEmission(c);
    return s;
}
