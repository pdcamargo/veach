// Photo mode's path tracer (docs/adr/0041-photo-mode.md): the reference renderer and ground truth.
// Unidirectional, progressive, unbiased: next-event estimation to punctual
// lights and emissive triangles, MIS against BSDF sampling, Russian roulette.
// Inline ray queries, so it runs as one compute dispatch. Compiled into each
// app's shader library with its materials (veach_add_scene_shaders).
// Parameter layout must match render::PathTraceParams (scene_renderer.cpp).

import veach.atmosphere;
import veach.bindless;
import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c), evaluateAlpha(kind, c)
import veach.brdf;
import veach.lights;
import veach.random;
import veach.scene;

struct Params {
    float4x4 inverse_view_projection;
    float4 camera_position_exposure;  ///< xyz, w = exposure scale (1 / (1.2 * 2^ev100))
    float4 sky_bounces;               ///< w = max bounces (xyz unused: the sky is `sky`, ADR 0038)
    DescriptorHandle<RWTexture2D<float4>> accumulation;  ///< rgba32f running mean of radiance (nits), w = samples
    DescriptorHandle<RaytracingAccelerationStructure> scene;
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<StructuredBuffer<LightRecord>> lights;
    DescriptorHandle<StructuredBuffer<EmissiveTriangle>> emissive;
    uint2 size;
    uint sample_index;  ///< samples already accumulated
    uint samples_per_frame;
    uint light_count;
    uint emissive_count;
    float emissive_total_weight;  ///< sum of area x weight over every emissive triangle
    uint scene_flags;  ///< SceneData::flags (veach/scene.slang)
    DescriptorHandle<RWStructuredBuffer<uint>> diagnostics;  ///< [0] lost history, [1] non-finite samples; never reset
    DescriptorHandle<StructuredBuffer<SkyFrame>> sky;  ///< the environment (ADR 0038)
};
[[vk::push_constant]] ConstantBuffer<Params> params;

// ---- the path -----------------------------------------------------------------------------------------------

/// The radiance along a camera ray, and the distance to its first hit (0 if it escaped: aerial perspective).
float3 radiance(float3 origin, float3 direction, inout Rng rng, out float first_hit) {
    float3 result = float3(0.0, 0.0, 0.0);
    float3 throughput = float3(1.0, 1.0, 1.0);
    float previous_bsdf_pdf = 0.0;  // 0 for the camera ray: emission it sees is not MIS-weighted
    first_hit = 0.0;
    // One trace() call site: Metal's compiler gives up on more (tools/metal-repros/path-tracer-second-trace).
    const uint max_bounces = uint(params.sky_bounces.w);
    const SkyFrame sky = params.sky[0];
    for (uint bounce = 0; bounce <= max_bounces; ++bounce) {
        const Trace t = trace(origin, direction, 1e30, rng);
        if (!t.hit) {
            // The sun is the directional light (next-event estimation): only camera rays see its disk.
            result += throughput * sky_radiance(sky, direction, bounce == 0);
            break;
        }
        if (bounce == 0) {
            first_hit = t.t;
        }
        const TriangleHit hit = surface_at(t.instance, t.primitive, t.bary, origin);
        MaterialContext ctx = { hit.surface, hit.material };
        const SurfaceSample s = evaluateSurface(hit.material.kind, ctx);

        // Emission found by BSDF sampling, weighted against light sampling.
        if (any(s.emission > 0.0) && (hit.front || (hit.material.flags & material_double_sided) != 0)) {
            float weight = 1.0;
            if (previous_bsdf_pdf > 0.0 && (hit.material.flags & material_emissive) != 0) {
                const float light_pdf = emissive_light_pdf(hit.material, hit.area, t.t, dot(hit.surface.geometric_normal, -direction));
                weight = power_heuristic(previous_bsdf_pdf, light_pdf);
            }
            result += throughput * s.emission * weight;
        }
        if (bounce == max_bounces) {
            break;
        }

        const Frame frame = frame_of(s.normal);
        const float3 v = frame.to_local(hit.surface.view);
        if (v.z <= 0.0) {
            break;
        }
        const Brdf b = brdf_of(s, v.z);
        result += throughput * sample_light(hit.surface.position, hit.surface.geometric_normal, frame, b, v, rng);

        const float3 l = brdf_sample(b, v, rng);
        float pdf;
        const float3 f = brdf_eval(b, v, l, pdf);
        if (pdf <= 0.0 || all(f <= 0.0)) {
            break;
        }
        throughput *= f / pdf;
        previous_bsdf_pdf = pdf;
        direction = frame.to_world(l);
        origin = offset_origin(hit.surface.position, hit.surface.geometric_normal, direction);

        if (bounce >= 3) {  // Russian roulette
            const float survive = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
            if (rng.next() > survive) {
                break;
            }
            throughput /= survive;
        }
    }
    return result;
}

float3 unproject(float2 ndc, float depth) {
    const float4 p = mul(params.inverse_view_projection, float4(ndc, depth, 1.0));
    return p.xyz / p.w;
}

/// NaN and infinity have every exponent bit set. Tested on the bits: Metal compiles with fast math, which may
/// fold isfinite() and isnan() to constants.
bool finite(float3 v) {
    const uint3 exponent = asuint(v) & 0x7f800000u;
    return all(exponent != 0x7f800000u);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace_pixels(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    veach_scene.tlas = params.scene;
    veach_scene.instances = params.instances;
    veach_scene.materials = params.materials;
    veach_scene.lights = params.lights;
    veach_scene.emissive = params.emissive;
    veach_scene.light_count = params.light_count;
    veach_scene.emissive_count = params.emissive_count;
    veach_scene.emissive_total_weight = params.emissive_total_weight;
    veach_scene.flags = params.scene_flags;
    float3 sum = float3(0.0, 0.0, 0.0);
    uint nonfinite = 0;
    for (uint k = 0; k < params.samples_per_frame; ++k) {
        const uint sample = params.sample_index + k;
        Rng rng = { pcg(id.x + pcg(id.y + pcg(sample))) };
        const float2 jitter = float2(rng.next(), rng.next());
        const float2 ndc = float2((float(id.x) + jitter.x) / float(params.size.x) * 2.0 - 1.0,
                                  1.0 - (float(id.y) + jitter.y) / float(params.size.y) * 2.0);
        const float3 near_point = unproject(ndc, 1.0);  // reverse-Z: the near plane is depth 1
        const float3 direction = normalize(unproject(ndc, 0.5) - near_point);
        float first_hit;
        float3 l = radiance(params.camera_position_exposure.xyz, direction, rng, first_hit);
        if (first_hit > 0.0) {  // aerial perspective, as the real-time renderer
            const float4 air = aerial_perspective(params.sky[0], direction, (float2(id.xy) + jitter) / float2(params.size),
                                                 first_hit);
            l = l * air.a + air.rgb;
        }
        // Left out so one bad sample cannot blacken a pixel for good, but counted: it is a bug (render.nonfinite_samples).
        if (finite(l)) {
            sum += l;
        } else {
            ++nonfinite;
        }
    }
    const float n = float(params.sample_index);
    const float k = float(params.samples_per_frame);
    float3 previous = float3(0.0, 0.0, 0.0);
    uint lost = 0;
    if (params.sample_index > 0) {
        // w holds the sample count this pixel last wrote: lost, stale or overwritten memory reads anything else
        // (render.history_lost). The mean stays as read, so the damage is visible too.
        const float4 stored = params.accumulation[id.xy];
        lost = stored.w != n || !finite(stored.xyz) || any(stored.xyz < 0.0) ? 1 : 0;
        previous = stored.xyz;
    }
    if (lost != 0) {
        InterlockedAdd(params.diagnostics[0], lost);
    }
    if (nonfinite != 0) {
        InterlockedAdd(params.diagnostics[1], nonfinite);
    }
    const float3 mean = (previous * n + sum) / (n + k);
    params.accumulation[id.xy] = float4(mean, n + k);  // the display pass shows it (ADR 0037)
}
