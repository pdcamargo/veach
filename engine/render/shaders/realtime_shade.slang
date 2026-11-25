// The real-time renderer (ADR 0037, ADR 0040): the passes that need the app's materials, compiled into each app's
// shader library (veach_add_scene_shaders). Every entry reads the frame record (veach/realtime.slang); its
// parameter block carries only that and a per-pass argument. Mirrors render::RealtimeParams.
//
//   surface:         resolves the visibility buffer into the surfaces, with motion and disocclusion (ADR 0040).
//   restir_temporal: ReSTIR DI's initial candidates, combined with last frame's reservoir (ADR 0041).
//   restir_spatial:  combined with neighbors' reservoirs.
//   shade:           the selected light sample with one shadow ray, emission and the ambient stopgap, into
//                    exposed radiance.
//   accumulate:      view_accumulated: the average of that radiance over frames while nothing changes.

import veach.bindless;
import veach.brdf;
import veach.lights;
import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c), evaluateAlpha(kind, c)
import veach.random;
import veach.realtime;
import veach.restir;
import veach.scene;
import veach.temporal;

static const uint no_geometry = 0xFFFFFFFFu;  ///< the visibility buffer's clear value

struct RealtimeParams {
    DescriptorHandle<StructuredBuffer<RealtimeFrame>> frame;
    uint argument;  ///< per pass
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<RealtimeParams> realtime_params;

float3 camera_ray(RealtimeFrame f, float2 pixel) {
    const float2 ndc = float2(pixel.x / float(f.render_size.x) * 2.0 - 1.0, 1.0 - pixel.y / float(f.render_size.y) * 2.0);
    const float4 near = mul(f.inverse_view_projection, float4(ndc, 1.0, 1.0));  // reverse-Z
    const float4 mid = mul(f.inverse_view_projection, float4(ndc, 0.5, 1.0));
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

// ---- surface -----------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void surface(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    use_scene(f);
    const uint2 visible_id = f.visibility.Load(int3(id.xy, 0));
    if (visible_id.x == no_geometry) {
        f.position_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.normal_out[id.xy] = float4(0.0, 0.0, 1.0, 1.0);
        f.geometric_out[id.xy] = float4(0.0, 0.0, 1.0, 0.0);
        f.diffuse_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.specular_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.emission_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.motion_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const InstanceRecord inst = f.instances[visible_id.x];
    const TriangleRef tri = meshlet_triangle(inst, visible_id.y >> 7, visible_id.y & 127u);
    const float3x3 corners = triangle_positions(inst, tri);
    const float3 p0 = to_world(inst.object_to_world, corners[0], 1.0);
    const float3 p1 = to_world(inst.object_to_world, corners[1], 1.0);
    const float3 p2 = to_world(inst.object_to_world, corners[2], 1.0);
    const float3 origin = f.camera_position.xyz;
    const float2 pixel = float2(id.xy) + 0.5 + f.jitter.xy;  // the sample the visibility pass rasterized
    const float2 bary = plane_barycentrics(origin, camera_ray(f, pixel), p0, p1, p2);
    TriangleHit hit = surface_of(inst, tri, bary, origin);
    // UV gradients from the neighboring pixels' rays on the same plane (no quad derivatives in compute).
    const float2 uv0 = fetch(inst, tri.vertices.x).uv;
    const float2 uv1 = fetch(inst, tri.vertices.y).uv;
    const float2 uv2 = fetch(inst, tri.vertices.z).uv;
    const float2 bx = plane_barycentrics(origin, camera_ray(f, pixel + float2(1.0, 0.0)), p0, p1, p2);
    const float2 by = plane_barycentrics(origin, camera_ray(f, pixel + float2(0.0, 1.0)), p0, p1, p2);
    const float2 uvx = uv0 * (1.0 - bx.x - bx.y) + uv1 * bx.x + uv2 * bx.y;
    const float2 uvy = uv0 * (1.0 - by.x - by.y) + uv1 * by.x + uv2 * by.y;
    hit.surface.uv_dx = uvx - hit.surface.uv;
    hit.surface.uv_dy = uvy - hit.surface.uv;

    MaterialContext ctx = { hit.surface, hit.material };
    const SurfaceSample s = evaluateSurface(hit.material.kind, ctx);
    const bool double_sided = (hit.material.flags & material_double_sided) != 0;
    const float3 emission = hit.front || double_sided ? s.emission : float3(0.0, 0.0, 0.0);
    const float3 p = hit.surface.position;
    const float3 current = to_pixel(f.unjittered_view_projection, p, f.render_size);  // z: linear view depth

    // Motion: this point moved by last frame's transforms, projected with last frame's camera (both unjittered).
    const float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    const float3 q0 = to_world(inst.previous_object_to_world, corners[0], 1.0);
    const float3 q1 = to_world(inst.previous_object_to_world, corners[1], 1.0);
    const float3 q2 = to_world(inst.previous_object_to_world, corners[2], 1.0);
    const float3 previous_point = q0 * w.x + q1 * w.y + q2 * w.z;
    float3 previous_geometric = normalize(cross(q1 - q0, q2 - q0));
    if (dot(previous_geometric, f.previous_camera_position.xyz - previous_point) < 0.0) {
        previous_geometric = -previous_geometric;  // faces last frame's camera, as the stored normals do
    }
    const float2 previous = to_pixel(f.previous_view_projection, previous_point, f.render_size).xy;
    const float2 motion = previous - current.xy;

    // Disocclusion, decided once here (veach/temporal.slang): which bilinear taps hold this surface last frame.
    uint valid_taps = 0;
    uint valid_neighbors = 0;
    if ((f.flags & frame_history_valid) != 0) {
        const int2 base = int2(floor(float2(id.xy) + motion));
        const int2 center = previous_pixel(id.xy, motion);
        // The 3x3 around the previous pixel contains the 2x2 bilinear footprint: test each pixel once.
        for (uint k = 0; k < 9; ++k) {
            const int2 q = center + int2(int(k % 3) - 1, int(k / 3) - 1);
            if (all(q >= 0) && all(q < int2(f.render_size))) {
                const float4 stored = f.previous_position.Load(int3(q, 0));
                const float3 stored_geometric = f.previous_geometric.Load(int3(q, 0)).xyz;
                if (same_surface(stored, stored_geometric, previous_point, previous_geometric, current.z)) {
                    valid_neighbors |= 1u << k;
                    const int2 tap = q - base;
                    if (all(tap >= 0) && all(tap <= 1)) {
                        valid_taps |= 1u << uint(tap.x + 2 * tap.y);
                    }
                }
            }
        }
    }

    const Brdf b = brdf_of(s, 1.0);
    f.position_out[id.xy] = float4(p, current.z);
    f.normal_out[id.xy] = float4(s.normal, s.roughness);
    f.geometric_out[id.xy] = float4(hit.surface.geometric_normal, s.metallic);
    f.diffuse_out[id.xy] = float4(b.diffuse, double_sided ? 1.0 : 0.0);
    f.specular_out[id.xy] = float4(b.f0, 0.0);
    f.emission_out[id.xy] = float4(emission, 0.0);
    f.motion_out[id.xy] = float4(motion, float(valid_neighbors), float(valid_taps));
}

// ---- ReSTIR DI (ADR 0041) ---------------------------------------------------------------------------------

static const uint max_spatial_samples = 8;
static const float spatial_radius = 24.0;  ///< render pixels

[shader("compute")]
[numthreads(8, 8, 1)]
void restir_temporal(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    use_scene(f);
    const uint index = id.y * f.render_size.x + id.x;
    const Domain d = domain_of(load_surface(f, int2(id.xy)), f.camera_position.xyz);
    if (!d.valid) {
        f.temporal_out[index] = pack(empty_reservoir());
        return;
    }
    Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 4u + 1u))) };
    Reservoir r = initial_reservoir(f, d, f.light_candidates, rng);
    const uint needed = frame_history_valid | frame_reservoirs_valid;
    if (f.temporal_reuse != 0 && (f.flags & needed) == needed) {
        const History h = history_of(id.xy, f.motion.Load(int3(id.xy, 0)));
        if (h.valid) {
            uint best = 0;  // the tap with the most weight
            for (uint k = 1; k < 4; ++k) {
                best = h.weights[k] > h.weights[best] ? k : best;
            }
            const int2 q = h.base + history_taps[best];
            Reservoir t = unpack(f.previous_reservoirs[q.y * int(f.render_size.x) + q.x]);
            if (t.M > 0.0) {
                t.M = min(t.M, temporal_confidence_cap * r.M);
                const Domain previous = domain_of(load_previous_surface(f, q), f.previous_camera_position.xyz);
                Pairwise p = pairwise_begin(r, d, 1, t.M, f.light_count);
                pairwise_add(p, t, previous, f.light_count, rng);
                r = pairwise_end(p, 1e30, rng);
            }
        }
    }
    f.temporal_out[index] = pack(r);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void restir_spatial(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    use_scene(f);
    const uint index = id.y * f.render_size.x + id.x;
    const GSurface s = load_surface(f, int2(id.xy));
    const Domain d = domain_of(s, f.camera_position.xyz);
    Reservoir r = unpack(f.temporal_reservoirs[index]);
    if (!d.valid || f.spatial_samples == 0) {
        f.final_out[index] = pack(d.valid ? r : empty_reservoir());
        return;
    }
    Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 4u + 2u))) };
    // Neighbors on a similar surface (a heuristic for efficiency only: the MIS weights keep any choice unbiased).
    int2 picks[max_spatial_samples];
    uint count = 0;
    float confidence = 0.0;
    for (uint i = 0; i < min(f.spatial_samples, max_spatial_samples); ++i) {
        const float radius = spatial_radius * sqrt(rng.next());
        const float angle = 2.0 * pi * rng.next();
        const int2 q = int2(id.xy) + int2(round(float2(cos(angle), sin(angle)) * radius));
        if (all(q == int2(id.xy)) || any(q < 0) || any(q >= int2(f.render_size))) {
            continue;
        }
        const float4 n_position = f.position.Load(int3(q, 0));
        const float3 n_normal = f.normal.Load(int3(q, 0)).xyz;
        if (n_position.w <= 0.0 || dot(n_normal, s.normal) < 0.9 || abs(n_position.w - s.depth) > 0.1 * s.depth) {
            continue;
        }
        picks[count] = q;
        confidence += f.temporal_reservoirs[q.y * int(f.render_size.x) + q.x].radiance_m.w;
        ++count;
    }
    Pairwise p = pairwise_begin(r, d, count, confidence, f.light_count);
    for (uint j = 0; j < count; ++j) {
        const int2 q = picks[j];
        const Reservoir other = unpack(f.temporal_reservoirs[q.y * int(f.render_size.x) + q.x]);
        pairwise_add(p, other, domain_of(load_lit_surface(f, q), f.camera_position.xyz), f.light_count, rng);
    }
    f.final_out[index] = pack(pairwise_end(p, 1e30, rng));
}

// ---- shade -------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void shade(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    use_scene(f);
    const float exposure = f.camera_position.w;
    const bool ambient = f.sky_ambient.w > 0.0;
    const GSurface s = load_surface(f, int2(id.xy));
    if (f.view == view_motion) {
        const float2 m = f.motion.Load(int3(id.xy, 0)).xy;
        f.radiance_out[id.xy] = float4(saturate(abs(m) * 0.25), s.valid ? 0.0 : 0.25, 1.0);
        return;
    }
    if (f.view == view_history) {
        const bool valid = history_of(id.xy, f.motion.Load(int3(id.xy, 0))).valid;
        f.radiance_out[id.xy] = float4(valid ? 0.0 : 1.0, valid ? 1.0 : 0.0, s.valid ? 0.0 : 1.0, 1.0);
        return;
    }
    if (!s.valid) {
        f.radiance_out[id.xy] = float4((ambient ? f.sky_ambient.xyz : float3(0.0, 0.0, 0.0)) * exposure, 1.0);
        return;
    }
    Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 4u + 3u))) };
    float3 radiance = s.emission;
    const Domain d = domain_of(s, f.camera_position.xyz);
    if (d.valid) {
        // Direct light: the reservoir's sample, with the pixel's one shadow ray (ADR 0041).
        const Reservoir r = unpack(f.final_reservoirs[id.y * f.render_size.x + id.x]);
        float3 l;
        float distance;
        const float3 c = contribution(r, d, f.light_count, l, distance);
        if (r.W > 0.0 && any(c > 0.0) && light_visible(d.p, d.g, l, distance, rng)) {
            radiance += c * r.W;
        }
        // STOPGAP until Candela (VCH-017): constant sky light on the diffuse lobe, darkened by short-range
        // ray-traced AO. A black sky contributes nothing, so its AO rays are skipped.
        if (ambient && f.ao_rays > 0 && any(f.sky_ambient.xyz > 0.0)) {
            uint open = 0;
            for (uint i = 0; i < f.ao_rays; ++i) {
                const float2 u = float2(rng.next(), rng.next());
                const float r2 = sqrt(u.x);
                const float3 dir = d.frame.to_world(float3(r2 * cos(2.0 * pi * u.y), r2 * sin(2.0 * pi * u.y), sqrt(1.0 - u.x)));
                open += visible(offset_origin(d.p, d.g, dir), dir, f.ao_radius, rng) ? 1 : 0;
            }
            radiance += f.sky_ambient.xyz * d.b.diffuse * (float(open) / float(f.ao_rays));
        }
    }
    f.radiance_out[id.xy] = float4(radiance * exposure, 1.0);
}

// ---- accumulate (view_accumulated) -----------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void accumulate(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    const float4 current = f.radiance.Load(int3(id.xy, 0));
    const float n = float(f.accumulated);
    const float4 average = f.accumulated == 0 ? current : (f.accumulation.Load(int3(id.xy, 0)) * n + current) / (n + 1.0);
    f.accumulation_out[id.xy] = average;
}
