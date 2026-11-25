// The reference path tracer (docs/adr/0034-lights-and-reference-path-tracer.md).
// Unidirectional, progressive, unbiased: next-event estimation to punctual
// lights and emissive triangles, MIS against BSDF sampling, Russian roulette.
// Inline ray queries, so it runs as one compute dispatch. Compiled into each
// app's shader library with its materials (veach_add_path_tracer).
// Parameter layout must match render::PathTraceParams (path_tracer.cpp).

import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c), evaluateAlpha(kind, c)

static const float pi = 3.14159265358979;

/// Mirrors render::InstanceRecord: 128 bytes per TLAS instance.
struct InstanceRecord {
    DescriptorHandle<StructuredBuffer<float>> vertices;  ///< 12 floats per vertex: position, normal, tangent, uv
    DescriptorHandle<StructuredBuffer<uint>> indices;
    DescriptorHandle<StructuredBuffer<uint>> slots;  ///< material slot per triangle
    uint material_base;                                 ///< slot 0's index in the material buffer
    uint flags;
    float4 object_to_world[3];  ///< rows of a 3x4 matrix
    float4 normal_to_world[3];  ///< rows of the inverse transpose
};

static const uint light_directional = 0;
static const uint light_point = 1;
static const uint light_spot = 2;

/// Mirrors render::LightRecord: a punctual light, 64 bytes.
struct LightRecord {
    float4 position;   ///< world
    float4 direction;  ///< where it shines (−Z of its transform)
    float4 radiance;   ///< color x intensity: lux (directional) or candela (point, spot)
    float cos_inner;
    float cos_outer;
    uint kind;
    uint pad;
};

/// Mirrors render::EmissiveTriangle: picked with probability proportional to area x material weight.
struct EmissiveTriangle {
    uint instance;
    uint primitive;
    float cdf;  ///< inclusive running sum, normalized to 1
    float pad;
};

struct Params {
    float4x4 inverse_view_projection;
    float4 camera_position_exposure;  ///< xyz, w = exposure scale (1 / (1.2 * 2^ev100))
    float4 sky_bounces;               ///< xyz sky radiance (nits), w = max bounces
    DescriptorHandle<RWTexture2D<float4>> output;        ///< scene color (rgba8, sRGB-encoded values)
    DescriptorHandle<RWTexture2D<float4>> accumulation;  ///< rgba32f running mean of linear radiance
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
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

// ---- random numbers: PCG hash (deterministic per pixel, sample and dimension) ------------------------------

uint pcg(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct Rng {
    uint state;
    [mutating] float next() {
        state = pcg(state);
        return float(state >> 8) * (1.0 / 16777216.0);
    }
};

// ---- geometry ---------------------------------------------------------------------------------------------

float3 to_world(float4 rows[3], float3 p, float w) {
    const float4 v = float4(p, w);
    return float3(dot(rows[0], v), dot(rows[1], v), dot(rows[2], v));
}

struct Corner {
    float3 position;
    float3 normal;
    float4 tangent;
    float2 uv;
};

Corner fetch(InstanceRecord inst, uint vertex) {
    const uint b = vertex * 12;
    Corner c;
    c.position = float3(inst.vertices[b], inst.vertices[b + 1], inst.vertices[b + 2]);
    c.normal = float3(inst.vertices[b + 3], inst.vertices[b + 4], inst.vertices[b + 5]);
    c.tangent = float4(inst.vertices[b + 6], inst.vertices[b + 7], inst.vertices[b + 8], inst.vertices[b + 9]);
    c.uv = float2(inst.vertices[b + 10], inst.vertices[b + 11]);
    return c;
}

struct TriangleHit {
    SurfaceInput surface;
    MaterialRecord material;
    float area;  ///< world space
    bool front;  ///< the ray hit the side the vertex winding faces
};

/// The surface at barycentrics (u, v) of a triangle, seen from `view_origin`.
TriangleHit surface_at(uint instance_index, uint primitive, float2 bary, float3 view_origin) {
    const InstanceRecord inst = params.instances[instance_index];
    const Corner c0 = fetch(inst, inst.indices[primitive * 3]);
    const Corner c1 = fetch(inst, inst.indices[primitive * 3 + 1]);
    const Corner c2 = fetch(inst, inst.indices[primitive * 3 + 2]);
    const float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    const float3 p0 = to_world(inst.object_to_world, c0.position, 1.0);
    const float3 p1 = to_world(inst.object_to_world, c1.position, 1.0);
    const float3 p2 = to_world(inst.object_to_world, c2.position, 1.0);
    TriangleHit hit;
    const float3 cross_edges = cross(p1 - p0, p2 - p0);
    hit.area = 0.5 * length(cross_edges);
    float3 geometric = normalize(cross_edges);
    hit.surface.position = p0 * w.x + p1 * w.y + p2 * w.z;
    hit.surface.view = normalize(view_origin - hit.surface.position);
    float3 normal = normalize(to_world(inst.normal_to_world, c0.normal * w.x + c1.normal * w.y + c2.normal * w.z, 0.0));
    const float4 t = c0.tangent * w.x + c1.tangent * w.y + c2.tangent * w.z;
    hit.surface.tangent = float4(to_world(inst.object_to_world, t.xyz, 0.0), t.w);
    hit.surface.uv = c0.uv * w.x + c1.uv * w.y + c2.uv * w.z;
    // Every surface is two-sided for the path tracer; `front` remembers which side was hit (emission is one-sided).
    hit.front = dot(geometric, hit.surface.view) >= 0.0;
    if (!hit.front) {
        geometric = -geometric;
        normal = -normal;
    }
    hit.surface.geometric_normal = geometric;
    hit.surface.normal = normal;
    hit.material = params.materials[inst.material_base + inst.slots[primitive]];
    return hit;
}

/// Alpha coverage at a candidate hit: mask tests the cutoff, blend is stochastic (ADR 0034). Deliberately
/// minimal (the material and one UV, no full surface): it runs inside the ray query's candidate loop, which
/// Metal's shader compiler cannot digest with the whole surface reconstruction inlined there.
bool accept_candidate(uint instance_index, uint primitive, float2 bary, inout Rng rng) {
    const InstanceRecord inst = params.instances[instance_index];
    const MaterialRecord material = params.materials[inst.material_base + inst.slots[primitive]];
    if ((material.flags & (material_alpha_mask | material_alpha_blend)) == 0) {
        return true;
    }
    const float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float2 uv = float2(0.0, 0.0);
    for (uint k = 0; k < 3; ++k) {
        const uint b = inst.indices[primitive * 3 + k] * 12 + 10;
        uv += float2(inst.vertices[b], inst.vertices[b + 1]) * w[k];
    }
    SurfaceInput surface = {};  // alpha needs only the UV
    surface.uv = uv;
    MaterialContext ctx = { surface, material };
    const float alpha = evaluateAlpha(material.kind, ctx);
    if ((material.flags & material_alpha_mask) != 0) {
        return alpha >= material.alpha_cutoff;
    }
    return rng.next() < alpha;
}

struct Trace {
    bool hit;
    uint instance;
    uint primitive;
    float2 bary;
    float t;
};

Trace trace(float3 origin, float3 direction, float tmax, inout Rng rng) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(params.scene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            accept_candidate(query.CandidateInstanceIndex(), query.CandidatePrimitiveIndex(),
                             query.CandidateTriangleBarycentrics(), rng)) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    Trace result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.instance = result.hit ? query.CommittedInstanceIndex() : 0;
    result.primitive = result.hit ? query.CommittedPrimitiveIndex() : 0;
    result.bary = result.hit ? query.CommittedTriangleBarycentrics() : float2(0.0, 0.0);
    result.t = result.hit ? query.CommittedRayT() : 0.0;
    return result;
}

/// Shadow rays start a little off the surface along the geometric normal, on the side they leave from.
float3 offset_origin(float3 p, float3 geometric, float3 direction) {
    return p + geometric * (dot(geometric, direction) >= 0.0 ? 1e-3 : -1e-3);
}

bool visible(float3 from, float3 to_direction, float distance, inout Rng rng) {
    return !trace(from, to_direction, distance, rng).hit;
}

// ---- the glTF metallic-roughness BRDF ------------------------------------------------------------------------

float luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

struct Frame {
    float3 t;
    float3 b;
    float3 n;
    float3 to_local(float3 v) { return float3(dot(v, t), dot(v, b), dot(v, n)); }
    float3 to_world(float3 v) { return v.x * t + v.y * b + v.z * n; }
};

Frame frame_of(float3 n) {  // Duff et al. 2017
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float b = n.x * n.y * a;
    Frame f;
    f.t = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    f.b = float3(b, s + n.y * n.y * a, -n.y);
    f.n = n;
    return f;
}

struct Brdf {
    float3 diffuse;  ///< base color x (1 - metallic)
    float3 f0;
    float alpha;  ///< roughness squared
    float specular_probability;
};

Brdf brdf_of(SurfaceSample s, float n_dot_v) {
    Brdf b;
    b.diffuse = s.base_color * (1.0 - s.metallic);
    b.f0 = lerp(float3(0.04, 0.04, 0.04), s.base_color, s.metallic);
    const float roughness = max(s.roughness, 0.02);
    b.alpha = roughness * roughness;
    const float spec = luminance(b.f0 + (1.0 - b.f0) * pow(1.0 - n_dot_v, 5.0));
    const float diff = luminance(b.diffuse);
    b.specular_probability = spec + diff > 0.0 ? clamp(spec / (spec + diff), 0.1, 0.9) : 0.5;
    return b;
}

float ggx_d(float n_dot_h, float a2) {
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (pi * d * d);
}

float smith_g1(float n_dot_x, float a2) {
    return 2.0 * n_dot_x / (n_dot_x + sqrt(a2 + (1.0 - a2) * n_dot_x * n_dot_x));
}

/// f(l) * cos(l) for local unit vectors (z = shading normal), and its sampling pdf (solid angle).
float3 brdf_eval(Brdf b, float3 v, float3 l, out float pdf) {
    pdf = 0.0;
    if (l.z <= 0.0 || v.z <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 h = normalize(v + l);
    const float a2 = b.alpha * b.alpha;
    const float d = ggx_d(h.z, a2);
    const float vis = 0.5 / (l.z * sqrt(v.z * v.z * (1.0 - a2) + a2) + v.z * sqrt(l.z * l.z * (1.0 - a2) + a2));
    const float3 f = b.f0 + (1.0 - b.f0) * pow(1.0 - saturate(dot(v, h)), 5.0);
    const float3 specular = d * vis * f;
    const float3 diffuse = (1.0 - f) * b.diffuse / pi;
    const float pdf_specular = smith_g1(v.z, a2) * d / (4.0 * v.z);  // VNDF
    const float pdf_diffuse = l.z / pi;
    pdf = b.specular_probability * pdf_specular + (1.0 - b.specular_probability) * pdf_diffuse;
    return (specular + diffuse) * l.z;
}

/// Heitz 2018: a visible-normal sample of GGX, in local space.
float3 sample_vndf(float3 v, float alpha, float2 u) {
    const float3 vh = normalize(float3(alpha * v.x, alpha * v.y, v.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x);
    const float phi = 2.0 * pi * u.y;
    const float p1 = r * cos(phi);
    const float s = 0.5 * (1.0 + vh.z);
    const float p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * r * sin(phi);
    const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

float3 brdf_sample(Brdf b, float3 v, inout Rng rng) {
    const float2 u = float2(rng.next(), rng.next());
    if (rng.next() < b.specular_probability) {
        return reflect(-v, sample_vndf(v, b.alpha, u));
    }
    const float r = sqrt(u.x);
    const float phi = 2.0 * pi * u.y;
    return float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
}

float power_heuristic(float a, float b) {
    return a * a / max(a * a + b * b, 1e-20);
}

// ---- lights ----------------------------------------------------------------------------------------------

float emissive_selection_probability() {
    return params.emissive_count == 0 ? 0.0 : (params.light_count == 0 ? 1.0 : 0.5);
}

/// Solid-angle pdf with which light sampling would have picked this point of an emissive triangle.
float emissive_light_pdf(MaterialRecord m, float area, float distance, float cos_light) {
    if (params.emissive_total_weight <= 0.0 || cos_light <= 0.0) {
        return 0.0;
    }
    const float pdf_area = emissive_selection_probability() * m.emissive_weight / params.emissive_total_weight;
    return pdf_area * distance * distance / cos_light;
}

float3 emission_of(TriangleHit hit) {
    if (!hit.front && (hit.material.flags & material_double_sided) == 0) {
        return float3(0.0, 0.0, 0.0);  // emission is one-sided
    }
    MaterialContext ctx = { hit.surface, hit.material };
    return evaluateSurface(hit.material.kind, ctx).emission;
}

/// A punctual light, picked uniformly: delta, so no MIS. Returns radiance x f x cos / pdf, and the shadow ray.
float3 sample_punctual(float3 p, Frame frame, Brdf b, float3 v_local, float p_punctual, inout Rng rng,
                       out float3 l, out float distance) {
    l = float3(0.0, 0.0, 1.0);
    distance = 0.0;
    if (params.light_count == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    const uint index = min(uint(rng.next() * float(params.light_count)), params.light_count - 1);
    const float select = p_punctual / float(params.light_count);
    const LightRecord light = params.lights[index];
    float3 irradiance = light.radiance.xyz;
    if (light.kind == light_directional) {
        l = -light.direction.xyz;
        distance = 1e30;
    } else {
        const float3 to_light = light.position.xyz - p;
        distance = length(to_light);
        l = to_light / distance;
        irradiance /= distance * distance;
        if (light.kind == light_spot) {
            const float cd = dot(light.direction.xyz, -l);
            const float falloff = saturate((cd - light.cos_outer) / max(light.cos_inner - light.cos_outer, 1e-4));
            irradiance *= falloff * falloff;  // KHR_lights_punctual
        }
    }
    float bsdf_pdf;
    const float3 f = brdf_eval(b, v_local, frame.to_local(l), bsdf_pdf);
    return irradiance * f / select;
}

/// One light sample, MIS-weighted for emissive triangles. Returns radiance x f x cos / pdf. There is one
/// shadow ray at the end: every trace() call site inlines the alpha test, and Metal's compiler gives up on many.
float3 sample_light(float3 p, float3 geometric, Frame frame, Brdf b, float3 v_local, inout Rng rng) {
    const float pick = rng.next();
    const float p_emissive = emissive_selection_probability();
    float3 contribution = float3(0.0, 0.0, 0.0);
    float3 l = float3(0.0, 0.0, 1.0);
    float distance = 0.0;
    if (pick < p_emissive) {
        // Emissive triangle: binary search the CDF, then a uniform point on it.
        const float u = rng.next();
        uint lo = 0;
        uint hi = params.emissive_count - 1;
        while (lo < hi) {
            const uint mid = (lo + hi) / 2;
            if (params.emissive[mid].cdf < u) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        const EmissiveTriangle tri = params.emissive[lo];
        const float su = sqrt(rng.next());
        const float2 bary = float2(1.0 - su, rng.next() * su);
        const TriangleHit light = surface_at(tri.instance, tri.primitive, bary, p);
        const float3 to_light = light.surface.position - p;
        distance = length(to_light);
        l = to_light / distance;
        const float cos_light = dot(light.surface.geometric_normal, -l);
        const float3 le = emission_of(light);
        const float light_pdf = emissive_light_pdf(light.material, light.area, distance, cos_light);
        if (light_pdf <= 0.0 || all(le <= 0.0)) {
            return float3(0.0, 0.0, 0.0);
        }
        float bsdf_pdf;
        const float3 f = brdf_eval(b, v_local, frame.to_local(l), bsdf_pdf);
        contribution = le * f * power_heuristic(light_pdf, bsdf_pdf) / light_pdf;
    } else {
        contribution = sample_punctual(p, frame, b, v_local, 1.0 - p_emissive, rng, l, distance);
    }
    if (all(contribution <= 0.0) || !visible(offset_origin(p, geometric, l), l, distance * 0.999, rng)) {
        return float3(0.0, 0.0, 0.0);
    }
    return contribution;
}

// ---- the path -----------------------------------------------------------------------------------------------

float3 radiance(float3 origin, float3 direction, inout Rng rng) {
    float3 result = float3(0.0, 0.0, 0.0);
    float3 throughput = float3(1.0, 1.0, 1.0);
    float previous_bsdf_pdf = 0.0;  // 0 for the camera ray: emission it sees is not MIS-weighted
    const uint max_bounces = uint(params.sky_bounces.w);
    for (uint bounce = 0; bounce <= max_bounces; ++bounce) {
        const Trace t = trace(origin, direction, 1e30, rng);
        if (!t.hit) {
            result += throughput * params.sky_bounces.xyz;
            break;
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

float linear_to_srgb(float c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace_pixels(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint k = 0; k < params.samples_per_frame; ++k) {
        const uint sample = params.sample_index + k;
        Rng rng = { pcg(id.x + pcg(id.y + pcg(sample))) };
        const float2 jitter = float2(rng.next(), rng.next());
        const float2 ndc = float2((float(id.x) + jitter.x) / float(params.size.x) * 2.0 - 1.0,
                                  1.0 - (float(id.y) + jitter.y) / float(params.size.y) * 2.0);
        const float3 near_point = unproject(ndc, 1.0);  // reverse-Z: the near plane is depth 1
        const float3 direction = normalize(unproject(ndc, 0.5) - near_point);
        const float3 l = radiance(params.camera_position_exposure.xyz, direction, rng);
        sum += all(isfinite(l)) ? l : float3(0.0, 0.0, 0.0);  // a NaN must not poison the running mean
    }
    const float n = float(params.sample_index);
    const float k = float(params.samples_per_frame);
    const float3 previous = params.sample_index == 0 ? float3(0.0, 0.0, 0.0) : params.accumulation[id.xy].xyz;
    const float3 mean = (previous * n + sum) / (n + k);
    params.accumulation[id.xy] = float4(mean, 1.0);
    const float3 exposed = mean * params.camera_position_exposure.w;
    params.output[id.xy] = float4(linear_to_srgb(exposed.r), linear_to_srgb(exposed.g), linear_to_srgb(exposed.b), 1.0);
}
