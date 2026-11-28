// Spike SP-16 (VCH-092), THROWAWAY: every raster pipeline of both renderers. One indexed indirect draw per part, its
// instances from the part's compacted list (sp16_cull.slang). Layout matches RasterParams in sp16_spike.cpp.
//
//   A, visibility buffer: vs_vis + vis_opaque_fs / vis_masked_fs write {instance, part << 20 | primitive} (rg32_uint).
//   B, forward: vs_forward + forward_fs (opaque), forward_masked_fs (alpha test, then the full shading),
//      forward_a2c_fs (MSAA: alpha to coverage); with a depth prepass, prepass_fs / prepass_masked_fs /
//      prepass_a2c_fs first, then forward_fs on everything with an equal depth test. sky_vs + sky_fs fill what is left.
//   Stats: count_fs counts the masked fragments that reach the shader and those that pass the alpha test.

import veach.bindless;
import sp16.common;

struct RasterParams {
    Scene scene;
    DescriptorHandle<RWStructuredBuffer<uint>> counters;  ///< count_fs: [0] fragments, [1] alpha-tested fragments kept
    uint part;
    uint flags;
};
[[vk::push_constant]] ConstantBuffer<RasterParams> params;

struct VisVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint instance : TEXCOORD1;
};

struct ForwardVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float3 world : TEXCOORD1;
};

struct Fetched {
    float3 world;
    float3 normal;
    float2 uv;
    uint instance;
};

/// The same code computes the clip position in every vertex shader, so a depth prepass and the shading pass that
/// tests it for equality agree bit for bit.
Fetched fetch_vertex(uint vertex, uint instance_in_list) {
    const Part part = params.scene.parts[params.part];
    Fetched f;
    f.instance = params.scene.lists[part.list_offset + instance_in_list];
    const Instance inst = params.scene.instances[f.instance];
    const Vertex v = params.scene.vertices[vertex];
    f.world = to_world(inst, v.position_u.xyz, 1.0);
    f.normal = to_world(inst, v.normal_v.xyz, 0.0);
    const Material m = params.scene.materials[part.material];
    f.uv = material_uv(m, float2(v.position_u.w, v.normal_v.w), f.world);
    return f;
}

float4 clip_of(float3 world) {
    return mul_columns(params.scene.frame[0].view_projection, float4(world, 1.0));
}

[shader("vertex")]
VisVertex vs_vis(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const Fetched f = fetch_vertex(vertex, instance);
    VisVertex o;
    o.position = clip_of(f.world);
    o.uv = f.uv;
    o.instance = f.instance;
    return o;
}

[shader("vertex")]
ForwardVertex vs_forward(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const Fetched f = fetch_vertex(vertex, instance);
    ForwardVertex o;
    o.position = clip_of(f.world);
    o.uv = f.uv;
    o.normal = f.normal;
    o.world = f.world;
    return o;
}

Material part_material() {
    return params.scene.materials[params.scene.parts[params.part].material];
}

bool alpha_kept(float2 uv) {
    const Material m = part_material();
    return alpha_of(params.scene, m, uv, ddx(uv), ddy(uv)) >= m.tint.a;
}

/// Alpha to coverage with the edge sharpened to a pixel wide (the cutoff maps to half coverage).
float coverage_alpha(float2 uv) {
    const Material m = part_material();
    const float a = alpha_of(params.scene, m, uv, ddx(uv), ddy(uv));
    return saturate((a - m.tint.a) / max(fwidth(a), 1e-4) + 0.5);
}

// ---- A: the visibility buffer ----------------------------------------------------------------------------------

[shader("fragment")]
uint2 vis_opaque_fs(VisVertex in, uint primitive: SV_PrimitiveID) : SV_Target {
    return uint2(in.instance, (params.part << 20) | primitive);
}

[shader("fragment")]
uint2 vis_masked_fs(VisVertex in, uint primitive: SV_PrimitiveID) : SV_Target {
    if (!alpha_kept(in.uv)) {
        discard;
    }
    return uint2(in.instance, (params.part << 20) | primitive);
}

// ---- B: forward ------------------------------------------------------------------------------------------------

float3 forward_shade(ForwardVertex in, bool front) {
    const Material m = part_material();
    ShadePoint p;
    p.position = in.world;
    p.normal = normalize(in.normal);
    const float3 g = normalize(cross(ddy(in.world), ddx(in.world)));
    p.geometric_normal = front ? g : -g;  // either winding; shade_point turns it toward the viewer
    p.uv = in.uv;
    p.uv_dx = ddx(in.uv);
    p.uv_dy = ddy(in.uv);
    p.vertex_uv = in.uv;
    return shade_point(params.scene, m, p);
}

[shader("fragment")]
float4 forward_fs(ForwardVertex in, bool front: SV_IsFrontFace) : SV_Target {
    return float4(forward_shade(in, front), 1.0);
}

[shader("fragment")]
float4 forward_masked_fs(ForwardVertex in, bool front: SV_IsFrontFace) : SV_Target {
    if (!alpha_kept(in.uv)) {
        discard;
    }
    return float4(forward_shade(in, front), 1.0);
}

[shader("fragment")]
float4 forward_a2c_fs(ForwardVertex in, bool front: SV_IsFrontFace) : SV_Target {
    const float a = coverage_alpha(in.uv);
    if (a <= 0.0) {
        discard;
    }
    return float4(forward_shade(in, front), a);
}

[shader("fragment")]
float4 prepass_fs(VisVertex in) : SV_Target {
    return float4(0.0, 0.0, 0.0, 1.0);
}

[shader("fragment")]
float4 prepass_masked_fs(VisVertex in) : SV_Target {
    if (!alpha_kept(in.uv)) {
        discard;
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}

[shader("fragment")]
float4 prepass_a2c_fs(VisVertex in) : SV_Target {
    const float a = coverage_alpha(in.uv);
    if (a <= 0.0) {
        discard;
    }
    return float4(0.0, 0.0, 0.0, a);
}

struct SkyVertex {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
SkyVertex sky_vs(uint vertex: SV_VertexID) {
    const float2 p = float2(float((vertex << 1) & 2), float(vertex & 2)) * 2.0 - 1.0;
    SkyVertex o;
    o.position = float4(p, 0.0, 1.0);  // the far plane (reverse-Z)
    o.ndc = p;
    return o;
}

[shader("fragment")]
float4 sky_fs(SkyVertex in) : SV_Target {
    const FrameData f = params.scene.frame[0];
    const float4 near = mul_columns(f.inverse_view_projection, float4(in.ndc, 1.0, 1.0));
    const float4 mid = mul_columns(f.inverse_view_projection, float4(in.ndc, 0.5, 1.0));
    const float3 direction = normalize(mid.xyz / mid.w - near.xyz / near.w);
    return float4(sky_radiance(f, direction) * f.camera.w, 1.0);
}

// ---- stats -----------------------------------------------------------------------------------------------------

[shader("fragment")]
uint2 count_fs(VisVertex in, uint primitive: SV_PrimitiveID) : SV_Target {
    InterlockedAdd(params.counters[0], 1u);
    if (!alpha_kept(in.uv)) {
        discard;
    }
    InterlockedAdd(params.counters[1], 1u);
    return uint2(in.instance, (params.part << 20) | primitive);
}
