// ReSTIR DI (ADR 0041): reservoirs of light samples, candidates from a power-weighted alias table over every light,
// and confidence-weighted pairwise MIS for combining reservoirs from other domains (last frame, neighbors). The
// target function is the unshadowed contribution; the only shadow ray is the shading ray, so the estimate stays
// unbiased (the comparison harness checks it against the path tracer's direct light).
//
// One light list: indices below light_count are punctual lights (LightRecord), the rest emissive triangles
// (EmissiveTriangle, index - light_count). A triangle sample caches its point, front normal and emitted radiance,
// which stay valid while the lights do not change (the renderer drops reservoirs when they do).

import veach.brdf;
import veach.lights;
import veach.material;
import veach.random;
import veach.realtime;
import veach.scene;

static const uint no_light = 0xFFFFFFFFu;
static const uint light_double_sided = 0x80000000u;  ///< Reservoir::light flag: the triangle emits from both sides
static const float temporal_confidence_cap = 20.0;   ///< last frame's reservoir counts at most 20 of this frame's

/// A reservoir in registers.
struct Reservoir {
    float3 position;  ///< a triangle sample's point
    uint light;       ///< unified light index | light_double_sided; no_light = empty
    float3 normal;    ///< a triangle sample's front-side geometric normal
    float W;          ///< unbiased contribution weight
    float3 radiance;  ///< a triangle sample's front-side emitted radiance, nits
    float M;          ///< confidence
};

PackedReservoir pack(Reservoir r) {
    PackedReservoir p;
    p.position_light = float4(r.position, asfloat(r.light));
    p.normal_w = float4(r.normal, r.W);
    p.radiance_m = float4(r.radiance, r.M);
    return p;
}

Reservoir unpack(PackedReservoir p) {
    Reservoir r;
    r.position = p.position_light.xyz;
    r.light = asuint(p.position_light.w);
    r.normal = p.normal_w.xyz;
    r.W = p.normal_w.w;
    r.radiance = p.radiance_m.xyz;
    r.M = p.radiance_m.w;
    return r;
}

Reservoir empty_reservoir() {
    Reservoir r;
    r.position = float3(0.0, 0.0, 0.0);
    r.light = no_light;
    r.normal = float3(0.0, 0.0, 1.0);
    r.W = 0.0;
    r.radiance = float3(0.0, 0.0, 0.0);
    r.M = 0.0;
    return r;
}

/// Where a reservoir's target function is evaluated: a pixel's surface, seen from its camera.
struct Domain {
    bool valid;
    float3 p;
    float3 g;  ///< geometric normal, for shadow-ray offsets
    Frame frame;
    Brdf b;
    float3 v;  ///< towards the camera, local
};

Domain domain_of(GSurface s, float3 camera) {
    Domain d;
    d.p = s.position;
    d.g = s.geometric;
    d.frame = frame_of(s.normal);
    d.v = d.frame.to_local(normalize(camera - s.position));
    d.valid = s.valid && d.v.z > 0.0;
    d.b = brdf_of_surface(s, max(d.v.z, 1e-4));
    return d;
}

/// The unshadowed contribution of a sample at a domain: radiance x f x cos (x the light's cos / d² for triangles),
/// and the direction and distance a shadow ray takes (distance >= 1e29: a directional light).
float3 contribution(Reservoir r, Domain d, uint light_count, out float3 l, out float distance) {
    l = float3(0.0, 0.0, 1.0);
    distance = 0.0;
    if (r.light == no_light || !d.valid) {
        return float3(0.0, 0.0, 0.0);
    }
    const uint index = r.light & ~light_double_sided;
    float pdf;
    if (index < light_count) {
        const float3 irradiance = punctual_irradiance(veach_scene.lights[index], d.p, l, distance);
        return irradiance * brdf_eval(d.b, d.v, d.frame.to_local(l), pdf);
    }
    const float3 to_light = r.position - d.p;
    distance = length(to_light);
    if (distance <= 1e-6) {
        return float3(0.0, 0.0, 0.0);
    }
    l = to_light / distance;
    float cos_light = dot(r.normal, -l);
    if ((r.light & light_double_sided) != 0) {
        cos_light = abs(cos_light);
    }
    if (cos_light <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    return r.radiance * brdf_eval(d.b, d.v, d.frame.to_local(l), pdf) * cos_light / (distance * distance);
}

/// contribution() split into the BRDF's diffuse and specular lobes (shading for the denoiser).
void contribution_lobes(Reservoir r, Domain d, uint light_count, out float3 diffuse, out float3 specular,
                        out float3 l, out float distance) {
    diffuse = float3(0.0, 0.0, 0.0);
    specular = float3(0.0, 0.0, 0.0);
    l = float3(0.0, 0.0, 1.0);
    distance = 0.0;
    if (r.light == no_light || !d.valid) {
        return;
    }
    const uint index = r.light & ~light_double_sided;
    float3 incoming;
    if (index < light_count) {
        incoming = punctual_irradiance(veach_scene.lights[index], d.p, l, distance);
    } else {
        const float3 to_light = r.position - d.p;
        distance = length(to_light);
        if (distance <= 1e-6) {
            return;
        }
        l = to_light / distance;
        float cos_light = dot(r.normal, -l);
        if ((r.light & light_double_sided) != 0) {
            cos_light = abs(cos_light);
        }
        if (cos_light <= 0.0) {
            return;
        }
        incoming = r.radiance * cos_light / (distance * distance);
    }
    float3 fd;
    float3 fs;
    brdf_eval_lobes(d.b, d.v, d.frame.to_local(l), fd, fs);
    diffuse = incoming * fd;
    specular = incoming * fs;
}

/// The target function: the contribution's luminance.
float target(Reservoir r, Domain d, uint light_count) {
    float3 l;
    float distance;
    return luminance(contribution(r, d, light_count, l, distance));
}

/// One candidate from the alias table, and its source pdf (selection x 1/area for a triangle's point).
Reservoir sample_light(RealtimeFrame f, Domain d, inout Rng rng, out float source_pdf) {
    Reservoir c = empty_reservoir();
    source_pdf = 0.0;
    const uint count = f.light_count + f.emissive_count;
    if (count == 0) {
        return c;
    }
    const uint column = min(uint(rng.next() * float(count)), count - 1);
    const AliasEntry entry = f.alias[column];
    const uint index = rng.next() < entry.threshold ? column : entry.alias;
    const float select = f.alias[index].pdf;
    if (select <= 0.0) {
        return c;
    }
    if (index < f.light_count) {
        c.light = index;
        source_pdf = select;
        return c;
    }
    const EmissiveTriangle tri = veach_scene.emissive[index - f.light_count];
    const float su = sqrt(rng.next());
    const float2 bary = float2(1.0 - su, rng.next() * su);
    TriangleHit hit = surface_at(tri.instance, tri.primitive, bary, d.p);
    if (hit.area <= 0.0) {
        return c;
    }
    const bool double_sided = (hit.material.flags & material_double_sided) != 0;
    c.light = index | (double_sided ? light_double_sided : 0u);
    c.position = hit.surface.position;
    c.normal = hit.front ? hit.surface.geometric_normal : -hit.surface.geometric_normal;  // the front side's
    hit.front = true;  // the front side's emission, whichever side p sees
    c.radiance = emission_of(hit);
    source_pdf = select / hit.area;
    return c;
}

/// Weighted reservoir sampling: keeps `candidate` with probability w / (sum so far).
struct Stream {
    Reservoir chosen;
    float chosen_target;  ///< the chosen sample's target in the domain being estimated
    float w_sum;
};

Stream empty_stream() {
    Stream s;
    s.chosen = empty_reservoir();
    s.chosen_target = 0.0;
    s.w_sum = 0.0;
    return s;
}

void stream_add(inout Stream s, Reservoir candidate, float w, float candidate_target, inout Rng rng) {
    if (!(w > 0.0)) {
        return;
    }
    s.w_sum += w;
    if (rng.next() * s.w_sum < w) {
        s.chosen = candidate;
        s.chosen_target = candidate_target;
    }
}

/// The streamed reservoir as an unbiased one: W = w_sum / target(chosen), M = confidence.
Reservoir stream_result(Stream s, float confidence) {
    Reservoir r = s.chosen;
    r.W = s.chosen_target > 0.0 ? s.w_sum / s.chosen_target : 0.0;
    if (!(r.W > 0.0) || !(r.W < 1e30)) {
        r = empty_reservoir();
    }
    r.M = confidence;
    return r;
}

/// Initial candidates by RIS: `count` alias-table samples, one kept. Confidence 1.
Reservoir initial_reservoir(RealtimeFrame f, Domain d, uint count, inout Rng rng) {
    Stream s = empty_stream();
    for (uint i = 0; i < count; ++i) {
        float source_pdf;
        const Reservoir c = sample_light(f, d, rng, source_pdf);
        if (source_pdf > 0.0) {
            const float t = target(c, d, f.light_count);
            stream_add(s, c, t / (source_pdf * float(count)), t, rng);
        }
    }
    return stream_result(s, 1.0);
}

/// Pairwise MIS (confidence-weighted): combines the canonical reservoir (this pixel's own) with `k` others whose
/// samples came from their own domains. Each pair (canonical, i) gets the share (M_i + M_c/k) / M_total; within it
/// the two weigh by M x target in their own domain. The weights sum to one for every sample, so the result is
/// unbiased wherever the canonical domain covers the integrand. Called incrementally: begin, add each other, end.
struct Pairwise {
    Stream stream;
    Reservoir canonical;
    Domain canonical_domain;
    float canonical_target;  ///< target(canonical sample, canonical domain)
    float canonical_weight;  ///< m_c accumulated over pairs
    float total_confidence;
    uint k;                  ///< how many others will be added
};

Pairwise pairwise_begin(Reservoir canonical, Domain d, uint k, float others_confidence, uint light_count) {
    Pairwise p;
    p.stream = empty_stream();
    p.canonical = canonical;
    p.canonical_domain = d;
    p.canonical_target = target(canonical, d, light_count);
    p.canonical_weight = 0.0;
    p.total_confidence = canonical.M + others_confidence;
    p.k = k;
    return p;
}

void pairwise_add(inout Pairwise p, Reservoir other, Domain other_domain, uint light_count, inout Rng rng) {
    const float mc = p.canonical.M / float(p.k);
    const float share = (other.M + mc) / p.total_confidence;
    // The other's sample.
    const float t_c = target(other, p.canonical_domain, light_count);
    const float t_i = target(other, other_domain, light_count);
    const float denominator = other.M * t_i + mc * t_c;
    const float m_i = denominator > 0.0 ? share * other.M * t_i / denominator : 0.0;
    stream_add(p.stream, other, m_i * t_c * other.W, t_c, rng);
    // The canonical sample's share of this pair.
    const float u_i = target(p.canonical, other_domain, light_count);
    const float denominator_c = other.M * u_i + mc * p.canonical_target;
    p.canonical_weight += denominator_c > 0.0 ? share * mc * p.canonical_target / denominator_c : share;
}

Reservoir pairwise_end(inout Pairwise p, float confidence_cap, inout Rng rng) {
    if (p.k == 0) {
        return p.canonical;
    }
    stream_add(p.stream, p.canonical, p.canonical_weight * p.canonical_target * p.canonical.W, p.canonical_target, rng);
    return stream_result(p.stream, min(p.total_confidence, confidence_cap));
}
