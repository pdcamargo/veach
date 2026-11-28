// Spike SP-16 (VCH-092), THROWAWAY: what both renderers of the vegetation field share, so they differ only in how
// they find the visible surface (docs/research/spikes/sp-16-visibility-vs-forward.md). The scene's records (layouts
// mirrored in apps/lab/src/sp16_spike.cpp), the vertex fetch, the material (trilinear sampling built from one texture
// per mip, because the Metal sampler has no mip filter today) and the lighting: the sun with the engine's glTF BRDF,
// foliage translucency, N point lights in one loop (what a light cluster would hold), a sky and ground ambient and
// height fog. Structured-buffer records use only 4-component and scalar members (no float3 padding questions).

import veach.bindless;
import veach.brdf;
import veach.material;

public static const uint part_masked = 1u;        ///< alpha-tested (discard below the material's cutoff)
public static const uint material_foliage = 1u;   ///< double-sided sheet, translucent, vertex normals bent
public static const uint material_ground_ao = 2u; ///< darkened toward the card's base (grass)
public static const uint material_world_uv = 4u;  ///< UV from world xz (the terrain)
public static const uint no_geometry = 0xFFFFFFFFu;

public struct Vertex {
    public float4 position_u;  ///< object space position, u
    public float4 normal_v;    ///< object space normal, v
};

public struct Instance {
    public float4 rows[3];  ///< object to world, rows of a 3x4 matrix (rotation x uniform scale, translation)
    public float4 sphere;   ///< world-space bounding sphere: center, radius
    public uint model;
    public uint pad0;
    public uint pad1;
    public uint pad2;
};

public struct Model {
    public uint first_part;
    public uint part_count;
    public float max_distance;  ///< culled beyond this distance from the camera (grass fades out in a real game)
    public uint pad;
};

public struct Part {
    public uint first_index;
    public uint index_count;
    public uint material;
    public uint flags;  ///< part_masked
    public uint list_offset;  ///< where this part's visible-instance list starts in `lists`
    public uint capacity;
    public uint pad0;
    public uint pad1;
};

public struct Material {
    public float4 tint;  ///< rgb multiplies the base color texture; a: alpha cutoff
    public float roughness;
    public float metallic;
    public float translucency;
    public float normal_strength;
    public uint base_mips;    ///< first entry in the texture table (one texture per mip level)
    public uint normal_mips;  ///< the normal map's
    public uint mip_count;
    public uint flags;  ///< material_*
    public float uv_scale;
    public float texture_size;  ///< level 0's width in texels (square textures)
    public uint pad0;
    public uint pad1;
};

public struct Light {
    public float4 position_range;  ///< render space position, range (m)
    public float4 color;           ///< rgb: intensity x color (candela)
};

/// The frame's camera and sky, one record. Matrices are glm's column-major columns.
public struct FrameData {
    public float4 view_projection[4];
    public float4 inverse_view_projection[4];
    public float4 previous_view_projection[4];  ///< for occlusion against last frame's depth
    public float4 planes[5];                    ///< left, right, bottom, top, near: dot(plane.xyz, p) + w >= 0 inside
    public float4 camera;                       ///< position; w = exposure
    public float4 sun_direction;                ///< toward the sun; w = near plane
    public float4 sun_color;                    ///< illuminance x color (lux)
    public float4 sky_color;                    ///< zenith radiance (nits); w = fog density (1/m)
    public float4 horizon_color;                ///< horizon radiance; w = fog height falloff (1/m)
    public float4 ground_color;                 ///< ambient radiance from below
    public uint4 size;                          ///< width, height, light count, flags
    public uint4 hiz;                           ///< level 0 width, height, level count, 1 = usable
    public uint4 hiz_offsets[4];                ///< 16 level offsets into the HiZ buffer (floats)
};

public float4 mul_columns(float4 c[4], float4 v) {
    return c[0] * v.x + c[1] * v.y + c[2] * v.z + c[3] * v.w;
}

public float3 to_world(Instance inst, float3 p, float w) {
    const float4 h = float4(p, w);
    return float3(dot(inst.rows[0], h), dot(inst.rows[1], h), dot(inst.rows[2], h));
}

/// One mip level of a material texture (a struct: Metal cannot point at bare texture handles).
public struct TextureRef {
    public DescriptorHandle<Texture2D<float4>> texture;
};

/// What both renderers need to reach the scene; every shader's parameter block starts with these.
public struct Scene {
    public DescriptorHandle<StructuredBuffer<FrameData>> frame;
    public DescriptorHandle<StructuredBuffer<Vertex>> vertices;
    public DescriptorHandle<StructuredBuffer<uint>> indices;  ///< absolute vertex indices
    public DescriptorHandle<StructuredBuffer<Instance>> instances;
    public DescriptorHandle<StructuredBuffer<Part>> parts;
    public DescriptorHandle<StructuredBuffer<uint>> lists;  ///< visible instances, per part (cull.slang)
    public DescriptorHandle<StructuredBuffer<Material>> materials;
    public DescriptorHandle<StructuredBuffer<TextureRef>> textures;
    public DescriptorHandle<SamplerState> sampler;  ///< linear, repeat, no mips (the RHI's)
    public DescriptorHandle<SamplerState> clamp_sampler;  ///< linear, clamped: foliage cards (no bleed across edges)
    public DescriptorHandle<StructuredBuffer<Light>> lights;
};

/// Trilinear filtering by hand: two bilinear taps from the two mips around the gradient's level of detail.
public float4 sample_trilinear(Scene s, uint first, uint count, float size, float2 uv, float2 dx, float2 dy,
                               bool clamped) {
    const float rho2 = max(dot(dx, dx), dot(dy, dy)) * size * size;
    const float lod = clamp(0.5 * log2(max(rho2, 1e-12)), 0.0, float(count - 1));
    const uint l0 = uint(lod);
    const uint l1 = min(l0 + 1, count - 1);
    const float f = lod - float(l0);
    const DescriptorHandle<Texture2D<float4>> t0 = s.textures[first + l0].texture;
    const DescriptorHandle<Texture2D<float4>> t1 = s.textures[first + l1].texture;
    const DescriptorHandle<SamplerState> sampler = clamped ? s.clamp_sampler : s.sampler;
    const float4 a = nonuniform(t0)->SampleLevel(nonuniform(sampler), uv, 0.0);
    const float4 b = nonuniform(t1)->SampleLevel(nonuniform(sampler), uv, 0.0);
    return lerp(a, b, f);
}

/// The material's UV for a vertex UV and world position.
public float2 material_uv(Material m, float2 uv, float3 world) {
    return (m.flags & material_world_uv) != 0 ? world.xz * m.uv_scale : uv;
}

/// Coverage of an alpha-tested texel: the alpha channel of the base color texture.
public float alpha_of(Scene s, Material m, float2 uv, float2 dx, float2 dy) {
    return sample_trilinear(s, m.base_mips, m.mip_count, m.texture_size, uv, dx, dy, true).a;
}

public struct ShadePoint {
    public float3 position;
    public float3 normal;            ///< interpolated vertex normal, unit, world space
    public float3 geometric_normal;  ///< the triangle's, unit, world space (either winding)
    public float2 uv;                ///< the material's UV and its screen gradients
    public float2 uv_dx;
    public float2 uv_dy;
    public float2 vertex_uv;  ///< the mesh's own UV (ground AO)
};

public float3 sky_radiance(FrameData f, float3 direction) {
    const float up = saturate(direction.y);
    float3 c = lerp(f.horizon_color.rgb, f.sky_color.rgb, sqrt(up));
    if (direction.y < 0.0) {
        c = lerp(f.horizon_color.rgb, f.ground_color.rgb, saturate(-direction.y * 4.0));
    }
    const float sun = dot(direction, f.sun_direction.xyz);
    c += f.sun_color.rgb * 1e-3 * pow(saturate(sun), 64.0);  // a soft glow, not a disk
    return c;
}

/// Exposed radiance leaving `p` toward the camera.
public float3 shade_point(Scene s, Material m, ShadePoint p) {
    const FrameData f = s.frame[0];
    const float3 camera = f.camera.xyz;
    const float3 view = normalize(camera - p.position);
    const bool foliage = (m.flags & material_foliage) != 0;

    const bool clamped = (m.flags & material_foliage) != 0;
    const float4 base =
        sample_trilinear(s, m.base_mips, m.mip_count, m.texture_size, p.uv, p.uv_dx, p.uv_dy, clamped);
    const float4 bump =
        sample_trilinear(s, m.normal_mips, m.mip_count, m.texture_size, p.uv, p.uv_dx, p.uv_dy, clamped);

    // A sheet seen from behind turns its normals toward the viewer (ADR 0078's double-sided rule, with the
    // vertex normals foliage is authored with).
    float3 n = p.normal;
    float3 g = p.geometric_normal;
    if (dot(g, view) < 0.0) {
        g = -g;
        if (foliage) {
            n = -n;
        }
    }
    // Tangent-space bump from the normal map, around n with a frame from the world axes (no tangents in the spike).
    const float3 t = normalize(abs(n.y) < 0.99 ? cross(float3(0.0, 1.0, 0.0), n) : cross(float3(1.0, 0.0, 0.0), n));
    const float3 b = cross(n, t);
    const float2 tilt = (bump.xy * 2.0 - 1.0) * m.normal_strength;
    n = normalize(n + t * tilt.x + b * tilt.y);
    if (dot(n, view) < 0.02) {
        n = normalize(n + view * (0.02 - dot(n, view)));
    }

    SurfaceSample surface;
    surface.base_color = base.rgb * m.tint.rgb;
    surface.alpha = 1.0;
    surface.normal = n;
    surface.metallic = m.metallic;
    surface.roughness = m.roughness;
    surface.emission = float3(0.0, 0.0, 0.0);
    const Frame frame = frame_of(n);
    const float3 v = frame.to_local(view);
    const Brdf brdf = brdf_of(surface, max(v.z, 1e-4));

    float ao = 1.0;
    if ((m.flags & material_ground_ao) != 0) {
        ao = lerp(1.0, 0.35, saturate(p.vertex_uv.y));
    }

    float pdf;
    float3 light = float3(0.0, 0.0, 0.0);
    const float3 sun = f.sun_direction.xyz;
    if (dot(g, sun) > 0.0 || foliage) {
        light += f.sun_color.rgb * brdf_eval(brdf, v, frame.to_local(sun), pdf);
    }
    if (foliage) {  // light through the leaf from behind
        light += f.sun_color.rgb * surface.base_color * m.translucency * saturate(-dot(n, sun)) / pi;
    }
    const uint light_count = f.size.z;
    for (uint i = 0; i < light_count; ++i) {
        const Light l = s.lights[i];
        const float3 d = l.position_range.xyz - p.position;
        const float d2 = dot(d, d);
        const float range2 = l.position_range.w * l.position_range.w;
        if (d2 >= range2) {
            continue;
        }
        const float3 dir = d * rsqrt(d2);
        const float window = saturate(1.0 - (d2 / range2) * (d2 / range2));
        light += l.color.rgb * (window * window / max(d2, 1e-4)) * brdf_eval(brdf, v, frame.to_local(dir), pdf);
    }
    const float3 ambient = lerp(f.ground_color.rgb, f.sky_color.rgb, n.y * 0.5 + 0.5);
    light += ambient * (brdf.diffuse + brdf.f0 * 0.25);
    light *= ao;

    // Height fog between the camera and the point.
    const float distance = length(camera - p.position);
    const float density = f.sky_color.w * exp(-f.horizon_color.w * max(p.position.y, 0.0));
    const float transmittance = exp(-density * distance);
    const float3 fog = f.horizon_color.rgb;
    return (light * transmittance + fog * (1.0 - transmittance)) * f.camera.w;
}
