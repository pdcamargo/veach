// The real-time renderer v0 (docs/adr/0037-realtime-renderer-visibility-buffer.md): a visibility buffer of
// meshlet triangles, then one compute pass that rebuilds each pixel's surface and lights it with the same
// materials, BRDF and light sampling as the path tracer. Compiled into each app's shader library with its
// materials (veach_add_scene_shaders). This file: the visibility pass. Its parameter layout matches render::VisibilityParams.

import veach.bindless;
import veach.brdf;
import veach.lights;
import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c), evaluateAlpha(kind, c)
import veach.random;
import veach.scene;

// ---- visibility -------------------------------------------------------------------------------------------

struct VisibilityParams {
    float4x4 view_projection;
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    uint instance;  ///< the instance this draw rasterizes: one draw per instance, one draw instance per meshlet
    uint pad0;
    uint2 pad1;
};
[[vk::push_constant]] ConstantBuffer<VisibilityParams> visibility;

struct VisibilityVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint2 id : TEXCOORD1;  ///< instance, meshlet << 7 | triangle
};

[shader("vertex")]
VisibilityVertex vis_vs(uint vertex: SV_VertexID, uint meshlet: SV_InstanceID) {
    veach_scene.instances = visibility.instances;
    const InstanceRecord inst = visibility.instances[visibility.instance];
    const uint triangle = vertex / 3;
    VisibilityVertex out;
    out.uv = float2(0.0, 0.0);
    out.id = uint2(visibility.instance, (meshlet << 7) | triangle);
    if (triangle >= nonuniform(inst.meshlets)[meshlet * 8 + 3]) {
        out.position = float4(0.0, 0.0, 0.0, 1.0);  // past the meshlet's triangles: all corners equal, no area
        return out;
    }
    const TriangleRef t = meshlet_triangle(inst, meshlet, triangle);
    const uint corner = vertex % 3;
    const Corner c = fetch(inst, corner == 0 ? t.vertices.x : (corner == 1 ? t.vertices.y : t.vertices.z));
    out.position = mul(visibility.view_projection, float4(to_world(inst.object_to_world, c.position, 1.0), 1.0));
    out.uv = c.uv;
    return out;
}

[shader("fragment")]
uint2 vis_fs(VisibilityVertex in) : SV_Target {
    const InstanceRecord inst = visibility.instances[in.id.x];
    if ((inst.flags & instance_has_alpha) != 0) {
        const uint slot = nonuniform(inst.meshlets)[(in.id.y >> 7) * 8 + 4];
        const MaterialRecord material = visibility.materials[inst.material_base + slot];
        if ((material.flags & (material_alpha_mask | material_alpha_blend)) != 0) {
            SurfaceInput surface = {};
            surface.uv = in.uv;
            MaterialContext ctx = { surface, material };
            // Blended materials are tested at 0.5 in v0 (no transparency yet).
            const float cutoff = (material.flags & material_alpha_mask) != 0 ? material.alpha_cutoff : 0.5;
            if (evaluateAlpha(material.kind, ctx) < cutoff) {
                discard;
            }
        }
    }
    return in.id;
}

