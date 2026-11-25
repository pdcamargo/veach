// The real-time renderer v0 (docs/adr/0037-realtime-renderer-visibility-buffer.md): a visibility buffer of
// meshlet triangles, then one compute pass that rebuilds each pixel's surface and lights it with the same
// materials, BRDF and light sampling as the path tracer. Compiled into each app's shader library with its
// materials (veach_add_scene_shaders). This file: the shading pass. Its parameter layout matches render::ShadeParams.

import veach.bindless;
import veach.brdf;
import veach.lights;
import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c), evaluateAlpha(kind, c)
import veach.random;
import veach.scene;

static const uint no_geometry = 0xFFFFFFFFu;  ///< the visibility buffer's clear value

// ---- shading ---------------------------------------------------------------------------------------------

struct ShadeParams {
    float4x4 inverse_view_projection;
    float4 camera_position;  ///< xyz
    float4 sky_ambient;      ///< xyz sky radiance (nits); w = 1 when the ambient stopgap is on
    DescriptorHandle<Texture2D<uint2>> visibility;
    DescriptorHandle<RWTexture2D<float4>> output;  ///< linear radiance, nits (rgba16f)
    DescriptorHandle<RaytracingAccelerationStructure> scene;
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<StructuredBuffer<LightRecord>> lights;
    DescriptorHandle<StructuredBuffer<EmissiveTriangle>> emissive;
    uint2 size;
    uint light_count;
    uint emissive_count;
    float emissive_total_weight;
    uint emissive_samples;  ///< light samples per pixel from emissive triangles
    uint ao_rays;           ///< stopgap AO: cosine rays per pixel
    float ao_radius;        ///< stopgap AO: how far an occluder counts, meters
    uint frame;             ///< seeds the random numbers
    uint debug_skip;        ///< debug_skip_* (veach/scene.slang): parts left out to measure what they cost
    uint scene_flags;       ///< SceneData::flags
    uint pad0;
    uint2 pad1;
};
[[vk::push_constant]] ConstantBuffer<ShadeParams> shade_params;

float3 camera_ray(float2 pixel) {
    const float2 ndc = float2(pixel.x / float(shade_params.size.x) * 2.0 - 1.0,
                              1.0 - pixel.y / float(shade_params.size.y) * 2.0);
    const float4 near = mul(shade_params.inverse_view_projection, float4(ndc, 1.0, 1.0));  // reverse-Z
    const float4 mid = mul(shade_params.inverse_view_projection, float4(ndc, 0.5, 1.0));
    return normalize(mid.xyz / mid.w - near.xyz / near.w);
}

/// Barycentrics (of p1, p2) where the ray meets the triangle's plane; outside the triangle too, for the
/// neighboring pixels' rays that give UV gradients.
float2 plane_barycentrics(float3 origin, float3 direction, float3 p0, float3 p1, float3 p2) {
    const float3 e1 = p1 - p0;
    const float3 e2 = p2 - p0;
    const float3 pv = cross(direction, e2);
    const float det = dot(e1, pv);
    const float inv = abs(det) > 1e-20 ? 1.0 / det : 0.0;
    const float3 tv = origin - p0;
    const float3 qv = cross(tv, e1);
    return float2(dot(tv, pv) * inv, dot(direction, qv) * inv);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void shade(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= shade_params.size)) {
        return;
    }
    veach_scene.tlas = shade_params.scene;
    veach_scene.instances = shade_params.instances;
    veach_scene.materials = shade_params.materials;
    veach_scene.lights = shade_params.lights;
    veach_scene.emissive = shade_params.emissive;
    veach_scene.light_count = shade_params.light_count;
    veach_scene.emissive_count = shade_params.emissive_count;
    veach_scene.emissive_total_weight = shade_params.emissive_total_weight;
    veach_scene.debug_skip = shade_params.debug_skip;
    veach_scene.flags = shade_params.scene_flags;

    const uint2 visible_id = shade_params.visibility.Load(int3(id.xy, 0));
    const bool ambient = shade_params.sky_ambient.w > 0.0;
    if (visible_id.x == no_geometry) {
        shade_params.output[id.xy] = float4(ambient ? shade_params.sky_ambient.xyz : float3(0.0, 0.0, 0.0), 1.0);
        return;
    }
    Rng rng = { pcg(id.x + pcg(id.y + pcg(shade_params.frame))) };
    const InstanceRecord inst = shade_params.instances[visible_id.x];
    const TriangleRef tri = meshlet_triangle(inst, visible_id.y >> 7, visible_id.y & 127u);
    const float3x3 corners = triangle_positions(inst, tri);
    const float3 p0 = to_world(inst.object_to_world, corners[0], 1.0);
    const float3 p1 = to_world(inst.object_to_world, corners[1], 1.0);
    const float3 p2 = to_world(inst.object_to_world, corners[2], 1.0);
    const float3 origin = shade_params.camera_position.xyz;
    const float2 pixel = float2(id.xy) + 0.5;
    const float2 bary = plane_barycentrics(origin, camera_ray(pixel), p0, p1, p2);
    TriangleHit hit = surface_of(inst, tri, bary, origin);
    // UV gradients from the neighboring pixels' rays on the same plane (no quad derivatives in compute).
    const float2 uv0 = fetch(inst, tri.vertices.x).uv;
    const float2 uv1 = fetch(inst, tri.vertices.y).uv;
    const float2 uv2 = fetch(inst, tri.vertices.z).uv;
    const float2 bx = plane_barycentrics(origin, camera_ray(pixel + float2(1.0, 0.0)), p0, p1, p2);
    const float2 by = plane_barycentrics(origin, camera_ray(pixel + float2(0.0, 1.0)), p0, p1, p2);
    const float2 uvx = uv0 * (1.0 - bx.x - bx.y) + uv1 * bx.x + uv2 * bx.y;
    const float2 uvy = uv0 * (1.0 - by.x - by.y) + uv1 * by.x + uv2 * by.y;
    hit.surface.uv_dx = uvx - hit.surface.uv;
    hit.surface.uv_dy = uvy - hit.surface.uv;

    MaterialContext ctx = { hit.surface, hit.material };
    const SurfaceSample s = evaluateSurface(hit.material.kind, ctx);
    float3 radiance = float3(0.0, 0.0, 0.0);
    if (hit.front || (hit.material.flags & material_double_sided) != 0) {
        radiance += s.emission;
    }
    const Frame frame = frame_of(s.normal);
    const float3 v = frame.to_local(hit.surface.view);
    if (v.z > 0.0) {
        const Brdf b = brdf_of(s, v.z);
        const float3 p = hit.surface.position;
        const float3 g = hit.surface.geometric_normal;
        radiance += direct_punctual(p, g, frame, b, v, rng);
        if (shade_params.emissive_samples > 0) {
            float3 emitted = float3(0.0, 0.0, 0.0);
            for (uint i = 0; i < shade_params.emissive_samples; ++i) {
                emitted += direct_emissive_sample(p, g, frame, b, v, rng);
            }
            radiance += emitted / float(shade_params.emissive_samples);
        }
        // STOPGAP until Candela (VCH-017): constant sky light on the diffuse lobe, darkened by short-range
        // ray-traced AO. Candela replaces it; at most a short-range AO stays.
        // A black sky contributes nothing, so its AO rays are skipped.
        if (ambient && shade_params.ao_rays > 0 && any(shade_params.sky_ambient.xyz > 0.0)) {
            uint open = 0;
            for (uint i = 0; i < shade_params.ao_rays; ++i) {
                const float2 u = float2(rng.next(), rng.next());
                const float r = sqrt(u.x);
                const float3 l =
                    frame.to_world(float3(r * cos(2.0 * pi * u.y), r * sin(2.0 * pi * u.y), sqrt(1.0 - u.x)));
                open += visible(offset_origin(p, g, l), l, shade_params.ao_radius, rng) ? 1 : 0;
            }
            radiance += shade_params.sky_ambient.xyz * b.diffuse * (float(open) / float(shade_params.ao_rays));
        }
    }
    shade_params.output[id.xy] = float4(radiance, 1.0);
}
