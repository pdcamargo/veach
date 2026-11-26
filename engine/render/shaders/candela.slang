// Candela (docs/specs/candela/spec.md, ADR 0051): the real-time renderer's diffuse indirect light. A ReSTIR GI final
// gather over a world-space radiance cache, compiled into each app's shader library with its materials
// (veach_add_scene_shaders). Every entry reads the real-time frame record (veach/realtime.slang); the parameter
// block mirrors render::RealtimeParams.
//
//   cache_update:  sparse diffuse paths (one pixel per 5x5 block) from the visible surfaces, each vertex's outgoing
//                  diffuse radiance added to its cell; the last vertex reads the cache for the bounces after it.
//   cache_resolve: per cell, this frame's mean blended into its value; idle cells evicted.
//   gather:        one cosine ray per `gi_rate` pixels, the radiance where it lands read from the cache (or the sky),
//                  as a GI reservoir combined with last frame's (temporal reuse).
//   spatial:       neighbors' reservoirs combined (spatial reuse), then the indirect light written for shading.

import veach.atmosphere;
import veach.bindless;
import veach.brdf;
import veach.diffuse_path;
import veach.material;
import veach.random;
import veach.radiance_cache;
import veach.realtime;
import veach.restir;
import veach.scene;
import veach.temporal;

struct RealtimeParams {
    DescriptorHandle<StructuredBuffer<RealtimeFrame>> frame;
    uint argument;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<RealtimeParams> realtime_params;

static const uint update_block = 5;          ///< one update path per this many pixels squared (SHaRC's 4%)
static const uint evict_after = 32;          ///< frames a cell keeps its value without new samples
static const float gi_temporal_cap = 16.0;   ///< M a reservoir carries into the next frame
static const float gi_spatial_radius = 30.0;  ///< render pixels
static const uint max_gi_spatial_samples = 4;

RadianceCache cache_of(RealtimeFrame f) {
    RadianceCache c;
    c.keys = f.cache_keys;
    c.accum = f.cache_accum;
    c.radiance = f.cache_radiance;
    c.camera = f.camera_position.xyz;
    c.capacity = f.cache_capacity;
    c.scale = f.cache_scale;
    c.cell_size = f.cache_cell_size;
    c.cell_scale = f.cache_cell_scale;
    c.max_frames = f.cache_max_frames;
    return c;
}

// ---- cache_update ------------------------------------------------------------------------------------------

static const uint max_update_vertices = 4;

[shader("compute")]
[numthreads(8, 8, 1)]
void cache_update(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 8u + 5u))) };
    const float2 u = float2(rng.next(), rng.next()) * float(update_block);
    const uint2 pixel = id.xy * update_block + min(uint2(u), uint2(update_block - 1, update_block - 1));
    if (any(pixel >= f.render_size)) {
        return;
    }
    use_scene(f);
    const GSurface s = load_lit_surface(f, int2(pixel));
    const Domain d = domain_of(s, f.camera_position.xyz);
    if (!d.valid) {
        return;
    }
    const RadianceCache c = cache_of(f);
    const SkyFrame sky = f.sky[0];
    // The path: the visible surface, then up to gi_cache_bounces diffuse surfaces; past the last one, the cache.
    DiffuseVertex vertices[max_update_vertices];
    float3 light[max_update_vertices];     // each vertex's light sample
    float3 incoming[max_update_vertices];  // what arrives along each vertex's bounce: emission, or the sky/cache
    vertices[0].position = s.position;
    vertices[0].geometric = s.geometric;
    vertices[0].normal = s.normal;
    vertices[0].albedo = s.diffuse;
    vertices[0].emission = float3(0.0, 0.0, 0.0);
    vertices[0].lit = true;
    const uint last = min(f.gi_cache_bounces, max_update_vertices - 1);
    uint count = 0;
    for (uint i = 0; i <= last; ++i) {
        const DiffuseVertex v = vertices[i];
        light[i] = diffuse_light(v, rng);
        incoming[i] = float3(0.0, 0.0, 0.0);
        count = i + 1;
        float pdf;
        const float3 direction = cosine_direction(frame_of(v.normal), rng, pdf);
        const float3 origin = offset_origin(v.position, v.geometric, direction);
        const Trace t = trace(origin, direction, 1e30, rng);
        if (!t.hit) {
            incoming[i] = sky_radiance(sky, direction, false);
            break;
        }
        const DiffuseVertex next = diffuse_vertex(t, origin, direction, pdf);
        incoming[i] = next.emission;
        if (!next.lit) {
            break;
        }
        if (i == last) {
            float3 cached;
            if (cache_lookup(c, next.position, next.geometric, cached)) {
                incoming[i] += cached;
            }
            break;
        }
        vertices[i + 1] = next;
    }
    // Back to front: each vertex's outgoing diffuse radiance, into its cell.
    float3 outgoing = float3(0.0, 0.0, 0.0);
    for (int i = int(count) - 1; i >= 0; --i) {
        const bool continues = i + 1 < int(count);
        outgoing = light[i] + vertices[i].albedo * (incoming[i] + (continues ? outgoing : float3(0.0, 0.0, 0.0)));
        cache_add(c, vertices[i].position, vertices[i].geometric, outgoing);
    }
}

// ---- cache_resolve -----------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void cache_resolve(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (id.x >= f.cache_capacity) {
        return;
    }
    resolve_cell(cache_of(f), id.x, evict_after);
}

// ---- GI reservoirs -----------------------------------------------------------------------------------------

/// A GI sample: a point the gather ray reached (or, for the sky, a direction), and the radiance it sends back.
struct GiReservoir {
    float3 point;  ///< or the direction, for the sky
    float3 normal;
    bool sky;
    float3 radiance;
    float w_sum;   ///< while combining; W once finished
    float M;
};

GiReservoir empty_gi() {
    GiReservoir r;
    r.point = float3(0.0, 0.0, 0.0);
    r.normal = float3(0.0, 0.0, 1.0);
    r.sky = false;
    r.radiance = float3(0.0, 0.0, 0.0);
    r.w_sum = 0.0;
    r.M = 0.0;
    return r;
}

float2 sign_not_zero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 oct_encode(float3 n) {
    const float2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? p : (1.0 - abs(p.yx)) * sign_not_zero(p);
}

float3 oct_decode(float2 e) {
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
    }
    return normalize(n);
}

PackedGiReservoir pack_gi(GiReservoir r) {
    PackedGiReservoir p;
    const uint2 e = uint2(saturate(oct_encode(r.normal) * 0.5 + 0.5) * 65535.0 + 0.5);
    p.point_normal = uint4(asuint(r.point), r.sky ? gi_sky_sample : (e.x | (e.y << 16)));
    const float3 l = min(r.radiance, 60000.0);
    p.radiance_mw = uint4(f32tof16(l.x) | (f32tof16(l.y) << 16), f32tof16(l.z) | (f32tof16(r.M) << 16),
                          asuint(r.w_sum), 0u);
    return p;
}

GiReservoir unpack_gi(PackedGiReservoir p) {
    GiReservoir r;
    r.point = asfloat(p.point_normal.xyz);
    r.sky = p.point_normal.w == gi_sky_sample;
    const uint w = p.point_normal.w;
    r.normal = r.sky ? float3(0.0, 0.0, 1.0) : oct_decode(float2(w & 0xffffu, w >> 16) / 65535.0 * 2.0 - 1.0);
    r.radiance = float3(f16tof32(p.radiance_mw.x), f16tof32(p.radiance_mw.x >> 16), f16tof32(p.radiance_mw.y));
    r.M = f16tof32(p.radiance_mw.y >> 16);
    r.w_sum = asfloat(p.radiance_mw.z);
    return r;
}

/// The sample's direction from receiver point p, and the cosine at p (shading normal n).
float3 gi_direction(GiReservoir r, float3 p) {
    return r.sky ? r.point : normalize(r.point - p);
}

/// ReSTIR GI's target at a receiver: the sample's luminance x cos at the receiver (the diffuse BRDF is constant).
float gi_target(GiReservoir r, float3 p, float3 n) {
    return luminance(r.radiance) * max(dot(n, gi_direction(r, p)), 0.0);
}

/// The sample's radiance read again from its point's cell (or the sky), so a light switched off shows within the
/// cache's own response.
void refresh(inout GiReservoir r, RadianceCache c, SkyFrame sky) {
    float3 now;
    if (r.sky) {
        r.radiance = sky_radiance(sky, r.point, false);
    } else if (cache_lookup(c, r.point, r.normal, now)) {
        r.radiance = now;
    }
}

/// Whether a sample can light receiver point p (shading normal n) at all: in front of it. Reused samples that cannot
/// are left out entirely; counting their M would darken the result.
bool gi_reaches(GiReservoir r, float3 p, float3 n) {
    return dot(n, gi_direction(r, p)) > 0.0;
}

/// Streams `other` (finished: w_sum holds W) into `r`, its target at the receiver times `jacobian`.
void gi_combine(inout GiReservoir r, GiReservoir other, float target, float jacobian, inout Rng rng) {
    if (other.M <= 0.0) {
        return;
    }
    const float w = target * other.w_sum * other.M * jacobian;
    r.w_sum += w;
    r.M += other.M;
    if (w > 0.0 && rng.next() * r.w_sum < w) {
        r.point = other.point;
        r.normal = other.normal;
        r.sky = other.sky;
        r.radiance = other.radiance;
    }
}

/// w_sum becomes W = w_sum / (M x target).
void gi_finish(inout GiReservoir r, float3 p, float3 n) {
    const float target = gi_target(r, p, n);
    r.w_sum = target > 0.0 && r.M > 0.0 ? r.w_sum / (r.M * target) : 0.0;
}

bool gathers_this_frame(uint2 pixel, uint frame, uint rate) {
    if (rate <= 1) {
        return true;
    }
    if (rate == 2) {
        return ((pixel.x + pixel.y + frame) & 1u) == 0u;
    }
    const uint order[4] = { 0u, 3u, 1u, 2u };
    return ((pixel.x & 1u) | ((pixel.y & 1u) << 1)) == order[frame & 3u];
}

// ---- gather ------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void gather(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    use_scene(f);
    const uint index = id.y * f.render_size.x + id.x;
    const GSurface s = load_lit_surface(f, int2(id.xy));
    const Domain d = domain_of(s, f.camera_position.xyz);
    if (!d.valid) {
        f.gi_temporal_out[index] = pack_gi(empty_gi());
        return;
    }
    Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 8u + 6u))) };
    const RadianceCache c = cache_of(f);
    const SkyFrame sky = f.sky[0];
    GiReservoir r = empty_gi();
    if (gathers_this_frame(id.xy, f.frame, f.gi_rate)) {
        float pdf;
        const float3 direction = cosine_direction(d.frame, rng, pdf);
        const float3 origin = offset_origin(d.p, d.g, direction);
        const Trace t = trace(origin, direction, 1e30, rng);
        GiReservoir sample = empty_gi();
        sample.M = 1.0;
        if (!t.hit) {
            sample.sky = true;
            sample.point = direction;
            sample.radiance = sky_radiance(sky, direction, false);
        } else {
            const TriangleHit hit = surface_at(t.instance, t.primitive, t.bary, origin);
            sample.point = hit.surface.position;
            sample.normal = hit.surface.geometric_normal;
            cache_lookup(c, sample.point, sample.normal, sample.radiance);
        }
        // Source density cos / pi: the weight is target / pdf.
        const float target = gi_target(sample, s.position, s.normal);
        sample.w_sum = pdf > 0.0 ? target / pdf : 0.0;
        r = sample;
        gi_finish(r, s.position, s.normal);
    }
    // Temporal reuse: last frame's reservoir of the same surface, its radiance read again from the cache.
    if ((f.flags & frame_history_valid) != 0 && f.gi_reservoirs_valid != 0 && f.temporal_reuse != 0) {
        const History h = history_of(id.xy, f.motion.Load(int3(id.xy, 0)));
        if (h.valid) {
            uint best = 0;
            for (uint k = 1; k < 4; ++k) {
                best = h.weights[k] > h.weights[best] ? k : best;
            }
            const int2 q = h.base + history_taps[best];
            GiReservoir previous = unpack_gi(f.gi_previous[q.y * int(f.render_size.x) + q.x]);
            previous.M = min(previous.M, gi_temporal_cap);
            refresh(previous, c, sky);
            if (gi_reaches(previous, s.position, s.normal)) {
                GiReservoir combined = empty_gi();
                gi_combine(combined, r, gi_target(r, s.position, s.normal), 1.0, rng);
                gi_combine(combined, previous, gi_target(previous, s.position, s.normal), 1.0, rng);
                gi_finish(combined, s.position, s.normal);
                r = combined;
            }
        }
    }
    f.gi_temporal_out[index] = pack_gi(r);
}

// ---- spatial -----------------------------------------------------------------------------------------------

/// The change of solid angle when a sample point seen from `from` is reused at `to`: cos at the point toward `to`
/// over cos toward `from`, times the squared distances the other way round. 1 for the sky.
float gi_jacobian(GiReservoir r, float3 from, float3 to) {
    if (r.sky) {
        return 1.0;
    }
    const float3 a = from - r.point;
    const float3 b = to - r.point;
    const float cos_from = abs(dot(r.normal, normalize(a)));
    const float cos_to = abs(dot(r.normal, normalize(b)));
    if (cos_from <= 0.0) {
        return 0.0;
    }
    const float j = (cos_to / cos_from) * (dot(a, a) / max(dot(b, b), 1e-8));
    return j > 10.0 ? 0.0 : j;  // a sample seen at a grazing angle from here: its weight would be a firefly
}

[shader("compute")]
[numthreads(8, 8, 1)]
void spatial(uint3 id: SV_DispatchThreadID) {
    const RealtimeFrame f = realtime_params.frame[0];
    if (any(id.xy >= f.render_size)) {
        return;
    }
    use_scene(f);
    const uint index = id.y * f.render_size.x + id.x;
    const GSurface s = load_lit_surface(f, int2(id.xy));
    const Domain d = domain_of(s, f.camera_position.xyz);
    if (!d.valid) {
        f.gi_final_out[index] = pack_gi(empty_gi());
        f.indirect_out[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    Rng rng = { pcg(id.x + pcg(id.y + pcg(f.frame * 8u + 7u))) };
    const GiReservoir center = unpack_gi(f.gi_temporal[index]);
    GiReservoir r = empty_gi();
    gi_combine(r, center, gi_target(center, s.position, s.normal), 1.0, rng);
    for (uint i = 0; i < min(f.gi_spatial_samples, max_gi_spatial_samples); ++i) {
        const float radius = gi_spatial_radius * sqrt(rng.next());
        const float angle = 2.0 * pi * rng.next();
        const int2 q = int2(id.xy) + int2(round(float2(cos(angle), sin(angle)) * radius));
        if (all(q == int2(id.xy)) || any(q < 0) || any(q >= int2(f.render_size))) {
            continue;
        }
        const float4 q_position = f.position.Load(int3(q, 0));
        const float3 q_normal = f.normal.Load(int3(q, 0)).xyz;
        if (q_position.w <= 0.0 || dot(q_normal, s.normal) < 0.9 || abs(q_position.w - s.depth) > 0.1 * s.depth) {
            continue;
        }
        const GiReservoir other = unpack_gi(f.gi_temporal[q.y * int(f.render_size.x) + q.x]);
        const float jacobian = gi_jacobian(other, q_position.xyz, s.position);
        if (jacobian > 0.0 && gi_reaches(other, s.position, s.normal)) {
            gi_combine(r, other, gi_target(other, s.position, s.normal), jacobian, rng);
        }
    }
    gi_finish(r, s.position, s.normal);
    f.gi_final_out[index] = pack_gi(r);
    // The cosine-weighted mean of the incoming radiance (what the diffuse albedo multiplies): L cos / pi x W.
    const float cos_theta = max(dot(s.normal, gi_direction(r, s.position)), 0.0);
    f.indirect_out[id.xy] = float4(r.radiance * cos_theta / pi * r.w_sum, 1.0);
}
