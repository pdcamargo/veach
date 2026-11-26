// The real-time renderer (ADR 0037, ADR 0040): the passes that need the app's materials, compiled into each app's
// shader library (veach_add_scene_shaders). Every entry reads the frame record (veach/realtime.slang); its
// parameter block carries only that and a per-pass argument. Mirrors render::RealtimeParams.
//
//   surface:         resolves the visibility buffer into the surfaces, with motion and disocclusion (ADR 0040).
//   restir_temporal: ReSTIR DI's initial candidates, combined with last frame's reservoir (ADR 0041).
//   restir_spatial:  combined with neighbors' reservoirs.
//   gi_ambient:      GI off (RealtimeLighting.gi): the ambient stopgap's sky light into the indirect light target.
//   shade:           the selected light sample with one shadow ray, emission and the indirect light, into exposed
//                    radiance.
//   accumulate:      view_accumulated: the average of that radiance over frames while nothing changes.
//   denoise_*:       the denoiser (ADR 0042): temporal accumulation, variance, an a-trous filter, remodulation.

import veach.atmosphere;
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
    // The visibility pass sampled this pixel's center through the jittered projection: the same ray, through the
    // jittered inverse. (In unjittered pixels the sample sits at the center minus the jitter.)
    const float2 pixel = float2(id.xy) + 0.5;
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

// ---- gi_ambient ----------------------------------------------------------------------------------------------

/// STOPGAP until Candela (VCH-017), GI off: sky light on the diffuse lobe through cosine-distributed rays, each seeing
/// the sky in its direction unless something within ao_radius blocks it (with an unlimited radius, the path tracer's
/// mode 3). A black sky contributes nothing, so its rays are skipped.
[shader("compute")]
[numthreads(8, 8, 1)]
void gi_ambient(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    use_scene(f);
    const GSurface s = load_lit_surface(f, int2(id.xy));
    const Domain d = domain_of(s, f.camera_position.xyz);
    const SkyFrame sky = f.sky[0];
    float3 sky_light = float3(0.0, 0.0, 0.0);
    if (d.valid && f.ao_rays > 0 && any(sky_ambient_radiance(sky) > 0.0)) {
        Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 4u + 2u))) };
        for (uint i = 0; i < f.ao_rays; ++i) {
            const float2 u = float2(rng.next(), rng.next());
            const float r2 = sqrt(u.x);
            const float3 dir = d.frame.to_world(float3(r2 * cos(2.0 * pi * u.y), r2 * sin(2.0 * pi * u.y), sqrt(1.0 - u.x)));
            if (visible(offset_origin(d.p, d.g, dir), dir, f.ao_radius, rng)) {
                sky_light += sky_radiance(sky, dir, false);
            }
        }
        sky_light /= float(f.ao_rays);
    }
    f.indirect_out[id.xy] = float4(sky_light, 1.0);
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
    const GSurface s = load_surface(f, int2(id.xy));
    if (f.view == view_motion) {
        const float2 m = f.motion.Load(int3(id.xy, 0)).xy;
        f.radiance_out[id.xy] = float4(saturate(abs(m) * 0.25), s.valid ? 0.0 : 0.25, 1.0);
        return;
    }
    if (f.view == view_history) {
        // Green: this point's history; yellow: only the same surface's next to it (a disocclusion within a pixel
        // of history); red: none.
        const History h = history_of(id.xy, f.motion.Load(int3(id.xy, 0)));
        f.radiance_out[id.xy] = float4(!h.valid || h.fallback ? 1.0 : 0.0, h.valid ? 1.0 : 0.0, s.valid ? 0.0 : 1.0, 1.0);
        return;
    }
    const SkyFrame sky = f.sky[0];
    if (!s.valid) {  // the sky, with the sun's disk (ADR 0050)
        const float3 direction = camera_ray(f, float2(id.xy) + 0.5);
        f.radiance_out[id.xy] = float4(sky_radiance(sky, direction, true) * exposure, 1.0);
        f.noisy_diffuse_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.noisy_specular_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 4u + 3u))) };
    float3 diffuse = float3(0.0, 0.0, 0.0);
    float3 specular = float3(0.0, 0.0, 0.0);
    const Domain d = domain_of(s, f.camera_position.xyz);
    if (d.valid) {
        // Direct light: the reservoir's sample, with the pixel's one shadow ray (ADR 0041).
        const Reservoir r = unpack(f.final_reservoirs[id.y * f.render_size.x + id.x]);
        float3 cd;
        float3 cs;
        float3 l;
        float distance;
        contribution_lobes(r, d, f.light_count, cd, cs, l, distance);
        const bool traced = r.W > 0.0 && (any(cd > 0.0) || any(cs > 0.0));
        if (traced && light_visible(d.p, d.g, l, distance, rng)) {
            diffuse += cd * r.W;
            specular += cs * r.W;
        }
        // Directional lights are not in ReSTIR's table: each is evaluated exactly, with its own shadow ray (a sized
        // one samples its disk), ADR 0046.
        uint directional_rays = 0;
        for (uint i = 0; i < f.light_count; ++i) {
            const LightRecord light = veach_scene.lights[i];
            if (light.kind != light_directional) {
                continue;
            }
            float3 sun_l;
            float sun_distance;
            const float3 irradiance = punctual_irradiance_sample(light, d.p, rng, sun_l, sun_distance);
            float3 fd;
            float3 fs;
            brdf_eval_lobes(d.b, d.v, d.frame.to_local(sun_l), fd, fs);
            if (any(fd > 0.0) || any(fs > 0.0)) {
                ++directional_rays;
                if (light_visible(d.p, d.g, sun_l, sun_distance, rng)) {
                    diffuse += irradiance * fd;
                    specular += irradiance * fs;
                }
            }
        }
        // Counted per wave, one atomic each: ReSTIR's shadow rays, the pixels lit, the directional lights' rays.
        const uint rays = WaveActiveCountBits(traced);
        const uint pixels = WaveActiveCountBits(true);
        const uint sun_rays = WaveActiveSum(directional_rays);
        if (WaveIsFirstLane()) {
            InterlockedAdd(f.counters[0], rays);
            InterlockedAdd(f.counters[1], pixels);
            InterlockedAdd(f.counters[2], sun_rays);
        }
        // Indirect light (the GI pass's) on the diffuse lobe.
        if (f.gi != gi_direct_only) {
            diffuse += f.indirect.Load(int3(id.xy, 0)).rgb * d.b.diffuse;
        }
    }
    // The noisy image, and its two lobes demodulated for the denoiser (ADR 0042); all exposed.
    const Demodulation m = demodulation_of(s, f.camera_position.xyz);
    // Aerial perspective (ADR 0050) on the image; the denoiser's signals stay without it (its combine adds it).
    const float4 air = aerial_perspective(sky, (float2(id.xy) + 0.5) / float2(f.render_size),
                                          length(s.position - f.camera_position.xyz));
    f.radiance_out[id.xy] = float4(((s.emission + diffuse + specular) * air.a + air.rgb) * exposure, 1.0);
    f.noisy_diffuse_out[id.xy] = float4(diffuse / m.diffuse * exposure, 0.0);
    f.noisy_specular_out[id.xy] = float4(specular / m.specular * exposure, 0.0);
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

// ---- the denoiser (ADR 0042) ------------------------------------------------------------------------------

static const float diffuse_max_history = 32.0;
static const float atrous_kernel[3] = { 0.25, 0.5, 0.25 };

float max_specular_history(float roughness) {
    return 8.0 + 24.0 * roughness;
}

/// Mean + 3 standard deviations of the 3x3 neighborhood's luminance: a brighter sample is a firefly.
float firefly_limit(DescriptorHandle<Texture2D<float4>> signal, int2 p, uint2 size) {
    float m1 = 0.0;
    float m2 = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(p + int2(x, y), int2(0, 0), int2(size) - 1);
            const float l = luminance(signal.Load(int3(q, 0)).rgb);
            m1 += l;
            m2 += l * l;
        }
    }
    m1 /= 9.0;
    return m1 + 3.0 * sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
}

float3 clamp_firefly(float3 c, float limit) {
    const float l = luminance(c);
    return l > limit ? c * (limit / max(l, 1e-6)) : c;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void denoise_temporal(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    const int2 p = int2(id.xy);
    if (f.position.Load(int3(p, 0)).w <= 0.0) {
        f.diffuse_history_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.specular_history_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.moments_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float3 diffuse = clamp_firefly(f.noisy_diffuse.Load(int3(p, 0)).rgb, firefly_limit(f.noisy_diffuse, p, f.render_size));
    const float3 specular = clamp_firefly(f.noisy_specular.Load(int3(p, 0)).rgb, firefly_limit(f.noisy_specular, p, f.render_size));
    const float roughness = f.normal.Load(int3(p, 0)).w;
    float4 hd = float4(0.0, 0.0, 0.0, 0.0);
    float4 hs = float4(0.0, 0.0, 0.0, 0.0);
    float4 hm = float4(0.0, 0.0, 0.0, 0.0);
    bool valid = false;
    if ((f.flags & frame_denoiser_history_valid) != 0) {
        const History h = history_of(id.xy, f.motion.Load(int3(p, 0)));
        valid = h.valid;
        for (uint k = 0; k < 4; ++k) {
            const float w = h.weights[k];
            if (w > 0.0) {
                const int2 q = h.base + history_taps[k];
                hd += f.previous_diffuse_history.Load(int3(q, 0)) * w;
                hs += f.previous_specular_history.Load(int3(q, 0)) * w;
                hm += f.previous_moments.Load(int3(q, 0)) * w;
            }
        }
    }
    // History is dropped at a disocclusion, never blended (ADR 0042).
    const float length_d = valid ? min(hd.a + 1.0, diffuse_max_history) : 1.0;
    const float length_s = valid ? min(hs.a + 1.0, max_specular_history(roughness)) : 1.0;
    const float alpha_d = 1.0 / length_d;
    const float alpha_s = 1.0 / length_s;
    const float ld = luminance(diffuse);
    const float ls = luminance(specular);
    f.diffuse_history_out[id.xy] = float4(lerp(hd.rgb, diffuse, alpha_d), length_d);
    f.specular_history_out[id.xy] = float4(lerp(hs.rgb, specular, alpha_s), length_s);
    f.moments_out[id.xy] = float4(lerp(hm.xy, float2(ld, ld * ld), alpha_d), lerp(hm.zw, float2(ls, ls * ls), alpha_s));
}

/// Luminance variance over the 7x7 neighborhood on the same surface: for pixels with too little history.
float2 spatial_variance(RealtimeFrame f, int2 p, float3 normal) {
    float4 m = float4(0.0, 0.0, 0.0, 0.0);
    float count = 0.0;
    for (int y = -3; y <= 3; ++y) {
        for (int x = -3; x <= 3; ++x) {
            const int2 q = p + int2(x, y);
            if (any(q < 0) || any(q >= int2(f.render_size)) || f.position.Load(int3(q, 0)).w <= 0.0 ||
                dot(f.normal.Load(int3(q, 0)).xyz, normal) < 0.9) {
                continue;
            }
            const float ld = luminance(f.diffuse_history.Load(int3(q, 0)).rgb);
            const float ls = luminance(f.specular_history.Load(int3(q, 0)).rgb);
            m += float4(ld, ld * ld, ls, ls * ls);
            count += 1.0;
        }
    }
    m /= max(count, 1.0);
    return max(float2(m.y - m.x * m.x, m.w - m.z * m.z), 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void denoise_variance(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    const int2 p = int2(id.xy);
    const float4 d = f.diffuse_history.Load(int3(p, 0));
    const float4 s = f.specular_history.Load(int3(p, 0));
    if (f.position.Load(int3(p, 0)).w <= 0.0) {  // no geometry: nothing to filter
        f.atrous_diffuse0_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        f.atrous_specular0_out[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float2 variance;
    if (min(d.a, s.a) < 4.0) {
        variance = spatial_variance(f, p, f.normal.Load(int3(p, 0)).xyz);  // too little history for moments
    } else {
        // The moments give the input's variance; what the spatial filter must tell apart is the accumulated mean's,
        // which shrinks with the frames in it (as in ReLAX): long-converged pixels keep their detail.
        const float4 m = f.moments.Load(int3(p, 0));
        variance = max(float2(m.y - m.x * m.x, m.w - m.z * m.z), 0.0) / float2(d.a, s.a);
    }
    f.atrous_diffuse0_out[id.xy] = float4(d.rgb, variance.x);
    f.atrous_specular0_out[id.xy] = float4(s.rgb, variance.y);
}

/// One a-trous iteration (argument: 0..4, step 2^argument): reads ping-pong buffer argument % 2, writes the other.
[shader("compute")]
[numthreads(8, 8, 1)]
void denoise_atrous(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    const uint iteration = realtime_params.argument;
    const bool from0 = iteration % 2 == 0;
    const int step = 1 << iteration;
    const int2 p = int2(id.xy);
    const float4 cd = from0 ? f.atrous_diffuse0.Load(int3(p, 0)) : f.atrous_diffuse1.Load(int3(p, 0));
    const float4 cs = from0 ? f.atrous_specular0.Load(int3(p, 0)) : f.atrous_specular1.Load(int3(p, 0));
    const float4 position = f.position.Load(int3(p, 0));
    float4 out_d = cd;
    float4 out_s = cs;
    if (position.w > 0.0) {
        const float4 normal = f.normal.Load(int3(p, 0));
        const float roughness = normal.w;
        const float ld = luminance(cd.rgb);
        const float ls = luminance(cs.rgb);
        const float sigma_d = 4.0 * sqrt(cd.a) + 1e-4;
        const float sigma_s = 4.0 * sqrt(cs.a) + 1e-4;
        const float specular_power = lerp(256.0, 16.0, saturate(roughness));  // smoother: sharper normal weight
        const bool mirror = roughness < 0.05 && iteration > 0;  // mirror-like specular: the first iteration only
        float3 sum_d = float3(0.0, 0.0, 0.0);
        float3 sum_s = float3(0.0, 0.0, 0.0);
        float w_d = 0.0;
        float w_s = 0.0;
        float var_d = 0.0;
        float var_s = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const int2 q = p + int2(x, y) * step;
                if (any(q < 0) || any(q >= int2(f.render_size))) {
                    continue;
                }
                const float4 qp = f.position.Load(int3(q, 0));
                if (qp.w <= 0.0) {
                    continue;
                }
                const float3 qn = f.normal.Load(int3(q, 0)).xyz;
                const float4 qd = from0 ? f.atrous_diffuse0.Load(int3(q, 0)) : f.atrous_diffuse1.Load(int3(q, 0));
                const float4 qs = from0 ? f.atrous_specular0.Load(int3(q, 0)) : f.atrous_specular1.Load(int3(q, 0));
                const float kernel = atrous_kernel[x + 1] * atrous_kernel[y + 1];
                const float plane = abs(dot(normal.xyz, qp.xyz - position.xyz));
                const float w_plane = exp(-plane / max(0.01 * position.w, 1e-4));
                const float n_dot = saturate(dot(normal.xyz, qn));
                const float wd = kernel * w_plane * pow(n_dot, 128.0) * exp(-abs(luminance(qd.rgb) - ld) / sigma_d);
                const float ws = kernel * w_plane * pow(n_dot, specular_power) * exp(-abs(luminance(qs.rgb) - ls) / sigma_s);
                sum_d += qd.rgb * wd;
                w_d += wd;
                var_d += wd * wd * qd.a;
                sum_s += qs.rgb * ws;
                w_s += ws;
                var_s += ws * ws * qs.a;
            }
        }
        if (w_d > 0.0) {
            out_d = float4(sum_d / w_d, var_d / (w_d * w_d));
        }
        if (w_s > 0.0 && !mirror) {
            out_s = float4(sum_s / w_s, var_s / (w_s * w_s));
        }
    }
    if (from0) {
        f.atrous_diffuse1_out[id.xy] = out_d;
        f.atrous_specular1_out[id.xy] = out_s;
    } else {
        f.atrous_diffuse0_out[id.xy] = out_d;
        f.atrous_specular0_out[id.xy] = out_s;
    }
}

/// Remodulation (argument: the ping-pong buffer the last a-trous iteration wrote): albedos back on, emission added.
[shader("compute")]
[numthreads(8, 8, 1)]
void denoise_combine(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    const int2 p = int2(id.xy);
    const GSurface s = load_surface(f, p);
    if (!s.valid) {
        f.denoised_out[id.xy] = f.radiance.Load(int3(p, 0));  // the sky, as shaded
        return;
    }
    const bool in0 = realtime_params.argument == 0;
    const float3 diffuse = in0 ? f.atrous_diffuse0.Load(int3(p, 0)).rgb : f.atrous_diffuse1.Load(int3(p, 0)).rgb;
    const float3 specular = in0 ? f.atrous_specular0.Load(int3(p, 0)).rgb : f.atrous_specular1.Load(int3(p, 0)).rgb;
    const Demodulation m = demodulation_of(s, f.camera_position.xyz);
    const float4 air = aerial_perspective(f.sky[0], (float2(id.xy) + 0.5) / float2(f.render_size),
                                          length(s.position - f.camera_position.xyz));
    const float3 lit = s.emission * f.camera_position.w + diffuse * m.diffuse + specular * m.specular;
    f.denoised_out[id.xy] = float4(lit * air.a + air.rgb * f.camera_position.w, 1.0);
}
