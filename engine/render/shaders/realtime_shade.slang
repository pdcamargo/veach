// The real-time renderer for now (docs/adr/0040-interim-realtime-path-visibility-buffer.md): one compute pass that
// rebuilds each pixel's surface from the visibility buffer and lights it with the same materials and BRDF as photo
// mode. Every punctual light, unshadowed; the sky's irradiance as ambient light on the diffuse lobe; emission; the sky
// behind geometry and aerial perspective in front of it. No ray tracing: it runs on any GPU the engine targets
// (ADR 0003). Compiled into each app's shader library with its materials (veach_add_scene_shaders). Its parameter
// layout matches render::ShadeParams (realtime_renderer.cpp).

import veach.atmosphere;
import veach.bindless;
import veach.brdf;
import veach.lights;
import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c)
import veach.scene;

static const uint no_geometry = 0xFFFFFFFFu;  ///< the visibility buffer's clear value

struct ShadeParams {
    float4x4 inverse_view_projection;
    float4 camera_position;  ///< xyz; w = exposure (1 / (1.2 * 2^ev100))
    DescriptorHandle<Texture2D<uint2>> visibility;  ///< instance, meshlet << 7 | triangle (realtime_visibility.slang)
    DescriptorHandle<RWTexture2D<float4>> radiance;  ///< rgba16f, exposed: the display pass applies AgX
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<StructuredBuffer<LightRecord>> lights;
    DescriptorHandle<StructuredBuffer<SkyFrame>> sky;  ///< ADR 0038
    uint2 size;
    uint light_count;
    uint flags;  ///< shade_auto_exposure, shade_no_fp16_clamp
    /// Spike SP-14 (VCH-090): with shade_auto_exposure, the pre-exposure is state[0].y (exposure.slang).
    DescriptorHandle<StructuredBuffer<float4>> exposure_state;
    uint2 pad;
};
[[vk::push_constant]] ConstantBuffer<ShadeParams> params;

static const uint shade_auto_exposure = 1u;
static const uint shade_no_fp16_clamp = 2u;
static const float fp16_max = 65504.0;

/// The exposed radiance as rgba16_float stores it, clamped to the fp16 maximum: the sun's disk (about 1.9e9 nits above
/// the air) exceeds it at any exposure below about EV100 14.5. Apple GPUs saturate such writes anyway; IEEE rounding
/// gives infinity. NaN is kept, for the exposure pass's check to count it (spike SP-14).
float4 storable(float3 c) {
    if ((params.flags & shade_no_fp16_clamp) == 0) {
        c = float3(c.x > fp16_max ? fp16_max : c.x, c.y > fp16_max ? fp16_max : c.y, c.z > fp16_max ? fp16_max : c.z);
    }
    return float4(c, 1.0);
}

float3 camera_ray(float2 pixel) {
    const float2 ndc = float2(pixel.x / float(params.size.x) * 2.0 - 1.0, 1.0 - pixel.y / float(params.size.y) * 2.0);
    const float4 near = mul(params.inverse_view_projection, float4(ndc, 1.0, 1.0));  // reverse-Z
    const float4 mid = mul(params.inverse_view_projection, float4(ndc, 0.5, 1.0));
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
    if (any(id.xy >= params.size)) {
        return;
    }
    veach_scene.instances = params.instances;
    veach_scene.materials = params.materials;
    veach_scene.lights = params.lights;
    veach_scene.light_count = params.light_count;
    veach_scene.emissive_count = 0;
    veach_scene.emissive_total_weight = 0.0;
    veach_scene.flags = 0;
    const SkyFrame sky = params.sky[0];
    const float exposure =
        (params.flags & shade_auto_exposure) != 0 ? params.exposure_state[0].y : params.camera_position.w;
    const float3 origin = params.camera_position.xyz;
    const float2 pixel = float2(id.xy) + 0.5;
    const float3 direction = camera_ray(pixel);

    const uint2 visible_id = params.visibility.Load(int3(id.xy, 0));
    if (visible_id.x == no_geometry) {  // the sky, with the sun's disk
        params.radiance[id.xy] = storable(sky_radiance_any(sky, direction, true) * exposure);
        return;
    }
    const InstanceRecord inst = params.instances[visible_id.x];
    const TriangleRef tri = meshlet_triangle(inst, visible_id.y >> 7, visible_id.y & 127u);
    const float3x3 corners = triangle_positions(inst, tri);
    const float3 p0 = to_world(inst.object_to_world, corners[0], 1.0);
    const float3 p1 = to_world(inst.object_to_world, corners[1], 1.0);
    const float3 p2 = to_world(inst.object_to_world, corners[2], 1.0);
    TriangleHit hit = surface_of(inst, tri, plane_barycentrics(origin, direction, p0, p1, p2), origin);
    // UV gradients from the neighboring pixels' rays on the same plane (no quad derivatives in compute).
    const float2 uv0 = fetch(inst, tri.vertices.x).uv;
    const float2 uv1 = fetch(inst, tri.vertices.y).uv;
    const float2 uv2 = fetch(inst, tri.vertices.z).uv;
    const float2 bx = plane_barycentrics(origin, camera_ray(pixel + float2(1.0, 0.0)), p0, p1, p2);
    const float2 by = plane_barycentrics(origin, camera_ray(pixel + float2(0.0, 1.0)), p0, p1, p2);
    hit.surface.uv_dx = uv0 * (1.0 - bx.x - bx.y) + uv1 * bx.x + uv2 * bx.y - hit.surface.uv;
    hit.surface.uv_dy = uv0 * (1.0 - by.x - by.y) + uv1 * by.x + uv2 * by.y - hit.surface.uv;

    MaterialContext ctx = { hit.surface, hit.material };
    const SurfaceSample s = evaluateSurface(hit.material.kind, ctx);
    const bool double_sided = (hit.material.flags & material_double_sided) != 0;
    float3 light = hit.front || double_sided ? s.emission : float3(0.0, 0.0, 0.0);
    // A normal map can turn the shading normal away from the camera at grazing angles: bend it back until the
    // surface faces the viewer, so such pixels are lit rather than black (photo mode averages them out instead).
    float3 normal = s.normal;
    const float n_dot_v = dot(normal, hit.surface.view);
    if (n_dot_v < 0.01) {
        normal = normalize(normal + hit.surface.view * (0.01 - n_dot_v));
    }
    const Frame frame = frame_of(normal);
    const float3 v = frame.to_local(hit.surface.view);
    if (v.z > 0.0) {
        const Brdf b = brdf_of(s, v.z);
        for (uint i = 0; i < params.light_count; ++i) {
            float3 l;
            float distance;
            const float3 irradiance = punctual_irradiance(veach_scene.lights[i], hit.surface.position, l, distance);
            if (dot(hit.surface.geometric_normal, l) <= 0.0) {
                continue;  // behind the surface itself: with no shadows, a normal map would otherwise leak it in
            }
            float pdf;
            light += irradiance * brdf_eval(b, v, frame.to_local(l), pdf);
        }
        light += sky_ambient_radiance(sky) * b.diffuse;
    }
    const float4 air = aerial_perspective(sky, pixel / float2(params.size), length(hit.surface.position - origin));
    params.radiance[id.xy] = storable((light * air.a + air.rgb) * exposure);
}
