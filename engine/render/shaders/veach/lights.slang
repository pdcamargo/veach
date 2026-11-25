// Lights as both renderers sample them (ADR 0034, ADR 0037): punctual lights exactly, emissive triangles in
// proportion to area x emission weight, one shadow ray per sample.

import veach.brdf;
import veach.material;
import veach.materials_generated;
import veach.random;
import veach.scene;

float emissive_selection_probability() {
    return veach_scene.emissive_count == 0 ? 0.0 : (veach_scene.light_count == 0 ? 1.0 : 0.5);
}

/// Solid-angle pdf with which light sampling would have picked this point of an emissive triangle.
float emissive_light_pdf(MaterialRecord m, float area, float distance, float cos_light) {
    if (veach_scene.emissive_total_weight <= 0.0 || cos_light <= 0.0) {
        return 0.0;
    }
    const float pdf_area = emissive_selection_probability() * m.emissive_weight / veach_scene.emissive_total_weight;
    return pdf_area * distance * distance / cos_light;
}

float3 emission_of(TriangleHit hit) {
    if (!hit.front && (hit.material.flags & material_double_sided) == 0) {
        return float3(0.0, 0.0, 0.0);  // emission is one-sided
    }
    MaterialContext ctx = { hit.surface, hit.material };
    return evaluateSurface(hit.material.kind, ctx).emission;
}

/// What a punctual light delivers at p, on a surface facing it (lux), and the direction and distance to it.
float3 punctual_irradiance(LightRecord light, float3 p, out float3 l, out float distance) {
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
    return irradiance;
}

/// A punctual light, picked uniformly: delta, so no MIS. Returns radiance x f x cos / pdf, and the shadow ray.
float3 sample_punctual(float3 p, Frame frame, Brdf b, float3 v_local, float p_punctual, inout Rng rng,
                       out float3 l, out float distance) {
    l = float3(0.0, 0.0, 1.0);
    distance = 0.0;
    if (veach_scene.light_count == 0) {
        return float3(0.0, 0.0, 0.0);
    }
    const uint index = min(uint(rng.next() * float(veach_scene.light_count)), veach_scene.light_count - 1);
    const float select = p_punctual / float(veach_scene.light_count);
    const float3 irradiance = punctual_irradiance(veach_scene.lights[index], p, l, distance);
    float bsdf_pdf;
    const float3 f = brdf_eval(b, v_local, frame.to_local(l), bsdf_pdf);
    return irradiance * f / select;
}

/// The emissive triangle whose CDF interval holds u (binary search).
uint pick_emissive(float u) {
    uint lo = 0;
    uint hi = veach_scene.emissive_count - 1;
    while (lo < hi) {
        const uint mid = (lo + hi) / 2;
        if (veach_scene.emissive[mid].cdf < u) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
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
        const EmissiveTriangle tri = veach_scene.emissive[pick_emissive(rng.next())];
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
    if (all(contribution <= 0.0) || !light_visible(p, geometric, l, distance, rng)) {
        return float3(0.0, 0.0, 0.0);
    }
    return contribution;
}

// ---- the real-time renderer's direct light (ADR 0037) ----------------------------------------------------

/// Every punctual light, exactly, each with one shadow ray (hard shadows).
float3 direct_punctual(float3 p, float3 geometric, Frame frame, Brdf b, float3 v_local, inout Rng rng) {
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < veach_scene.light_count; ++i) {
        float3 l;
        float distance;
        const float3 irradiance = punctual_irradiance(veach_scene.lights[i], p, l, distance);
        float bsdf_pdf;
        const float3 c = irradiance * brdf_eval(b, v_local, frame.to_local(l), bsdf_pdf);
        if (any(c > 0.0) && light_visible(p, geometric, l, distance, rng)) {
            sum += c;
        }
    }
    return sum;
}

/// One sample of the emissive triangles (area x emission weight, a uniform point on the triangle) with a shadow
/// ray: light sampling only, so unbiased without MIS. Average several for less noise.
float3 direct_emissive_sample(float3 p, float3 geometric, Frame frame, Brdf b, float3 v_local, inout Rng rng) {
    if (veach_scene.emissive_count == 0 || veach_scene.emissive_total_weight <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const EmissiveTriangle tri = veach_scene.emissive[pick_emissive(rng.next())];
    const float su = sqrt(rng.next());
    const float2 bary = float2(1.0 - su, rng.next() * su);
    const TriangleHit light = surface_at(tri.instance, tri.primitive, bary, p);
    const float3 to_light = light.surface.position - p;
    const float distance = length(to_light);
    const float3 l = to_light / distance;
    const float cos_light = dot(light.surface.geometric_normal, -l);
    const float3 le = emission_of(light);
    // emissive_light_pdf includes the path tracer's choice between punctual and emissive; here it is certain.
    const float pdf = emissive_light_pdf(light.material, light.area, distance, cos_light) /
                      emissive_selection_probability();
    if (pdf <= 0.0 || all(le <= 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    float bsdf_pdf;
    const float3 c = le * brdf_eval(b, v_local, frame.to_local(l), bsdf_pdf) / pdf;
    if (all(c <= 0.0) || !light_visible(p, geometric, l, distance, rng)) {
        return float3(0.0, 0.0, 0.0);
    }
    return c;
}
