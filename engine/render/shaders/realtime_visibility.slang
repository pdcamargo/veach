// The real-time renderer's visibility pass (docs/adr/0093-gpu-driven-core.md): the meshlets culling kept, from its
// compacted draw lists (gpu_cull.slang), one indirect draw per list and one draw instance per meshlet, by vertex
// pulling. Each pixel stores {instance, meshlet << 7 | triangle} in an rg32_uint target, with reverse-Z depth, and
// its motion: where the surface was on screen last frame, as an offset in UV (previous minus current), for temporal
// accumulation (ADR 0061). Opaque lists draw with vis_fs, alpha-tested ones with vis_fs_masked. Every surface is
// rasterized two-sided, as in photo mode; which side is shaded follows the material (ADR 0078). Compiled into each
// app's shader library with its materials (veach_add_scene_shaders). Layout matches render::VisibilityParams.

import veach.bindless;
import veach.material;
import veach.materials_generated;  // evaluateAlpha(kind, c)
import veach.scene;

struct VisibilityParams {
    float4x4 view_projection;
    float4x4 previous_view_projection;  ///< last frame's without jitter, in this frame's render space
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<StructuredBuffer<uint2>> visible;  ///< the draw lists: instance, meshlet
    float2 inverse_size;                                ///< 1 / the target's size in pixels
    uint list_offset;                                   ///< this draw's list in `visible`
    uint pad0;
    float2 jitter;  ///< UV: how far view_projection's sub-pixel jitter moves every point this frame (ADR 0061)
};
[[vk::push_constant]] ConstantBuffer<VisibilityParams> visibility;

struct VisibilityVertex {
    float4 position : SV_Position;
    float4 previous : TEXCOORD0;  ///< clip position last frame
    float2 uv : TEXCOORD1;
    nointerpolation uint2 id : TEXCOORD2;  ///< instance, meshlet << 7 | triangle
};

struct VisibilityTargets {
    uint2 id : SV_Target0;
    float2 motion : SV_Target1;
};

[shader("vertex")]
VisibilityVertex vis_vs(uint vertex: SV_VertexID, uint draw_instance: SV_InstanceID) {
    veach_scene.instances = visibility.instances;
    const uint2 entry = visibility.visible[visibility.list_offset + draw_instance];
    const InstanceRecord inst = visibility.instances[entry.x];
    const uint meshlet = entry.y;
    const uint triangle = vertex / 3;
    VisibilityVertex out;
    out.uv = float2(0.0, 0.0);
    out.previous = float4(0.0, 0.0, 0.0, 1.0);
    out.id = uint2(entry.x, (meshlet << 7) | triangle);
    if (triangle >= nonuniform(inst.meshlets)[meshlet * 8 + 3]) {
        out.position = float4(0.0, 0.0, 0.0, 1.0);  // past the meshlet's triangles: all corners equal, no area
        return out;
    }
    const TriangleRef t = meshlet_triangle(inst, meshlet, triangle);
    const uint corner = vertex % 3;
    const Corner c = fetch(inst, corner == 0 ? t.vertices.x : (corner == 1 ? t.vertices.y : t.vertices.z));
    out.position = mul(visibility.view_projection, float4(to_world(inst.object_to_world, c.position, 1.0), 1.0));
    out.previous =
        mul(visibility.previous_view_projection, float4(to_world(inst.previous_object_to_world, c.position, 1.0), 1.0));
    out.uv = c.uv;
    return out;
}

/// Previous minus current position on screen, in UV (y down), without this frame's jitter: a still surface under a
/// still camera has none.
float2 motion_of(VisibilityVertex in) {
    const float2 ndc = in.previous.xy / in.previous.w;
    const float2 previous = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return previous - (in.position.xy * visibility.inverse_size - visibility.jitter);
}

[shader("fragment")]
VisibilityTargets vis_fs(VisibilityVertex in) {
    VisibilityTargets out;
    out.id = in.id;
    out.motion = motion_of(in);
    return out;
}

/// Alpha-tested lists: masked materials test their cutoff, blended ones 0.5 (no transparency yet).
[shader("fragment")]
VisibilityTargets vis_fs_masked(VisibilityVertex in) {
    const InstanceRecord inst = visibility.instances[in.id.x];
    const uint slot = nonuniform(inst.meshlets)[(in.id.y >> 7) * 8 + 4];
    const MaterialRecord material = visibility.materials[inst.material_base + slot];
    SurfaceInput surface = {};
    surface.uv = in.uv;
    MaterialContext ctx = { surface, material };
    const float cutoff = (material.flags & material_alpha_mask) != 0 ? material.alpha_cutoff : 0.5;
    if (evaluateAlpha(material.kind, ctx) < cutoff) {
        discard;
    }
    VisibilityTargets out;
    out.id = in.id;
    out.motion = motion_of(in);
    return out;
}
