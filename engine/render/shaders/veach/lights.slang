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

// ---- lights with a size (docs/specs/soft-shadows/spec.md) -----------------------------------------------------
// A point or spot light with a radius is a sphere emitting its intensity uniformly (radiance I / (pi r^2)); a
// directional light with an angular radius is a disk of the same illuminance. A sample of its extent does not
// depend on the receiver: a point on the sphere (uniform over its area) or a direction in the disk's cone (uniform
// over its solid angle), so ReSTIR can reuse it like an emissive triangle's point.

/// A sample of a sized light: `point` and outward `normal` on a sphere, or `normal` = the direction toward a disk.
void sample_light_extent(LightRecord light, float2 u, out float3 point, out float3 normal) {
    if (light.kind == light_directional) {
        const float cos_theta = 1.0 - u.x * (1.0 - cos(light.size));
        const float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
        const Frame f = frame_of(-light.direction.xyz);
        normal = f.to_world(float3(sin_theta * cos(2.0 * pi * u.y), sin_theta * sin(2.0 * pi * u.y), cos_theta));
        point = float3(0.0, 0.0, 0.0);
        return;
    }
    const float z = 1.0 - 2.0 * u.x;
    const float r = sqrt(max(1.0 - z * z, 0.0));
    normal = float3(r * cos(2.0 * pi * u.y), r * sin(2.0 * pi * u.y), z);
    point = light.position.xyz + light.size * normal;
}

/// sample_light_extent's density: per area (sphere) or per solid angle (disk).
float light_extent_pdf(LightRecord light) {
    if (light.kind == light_directional) {
        return 1.0 / (2.0 * pi * max(1.0 - cos(light.size), 1e-12));
    }
    return 1.0 / (4.0 * pi * light.size * light.size);
}

/// What a sized light's sample sends toward p, before the BSDF: radiance x cos at the light / d^2 for a sphere's point
/// (area measure), radiance for a disk's direction (solid angle measure). And the shadow ray's direction and length.
float3 light_extent_incoming(LightRecord light, float3 point, float3 normal, float3 p, out float3 l, out float distance) {
    if (light.kind == light_directional) {
        l = normal;
        distance = 1e30;
        return light.radiance.xyz * light_extent_pdf(light);  // illuminance / solid angle
    }
    const float3 to_light = point - p;
    distance = length(to_light);
    l = distance > 1e-6 ? to_light / distance : float3(0.0, 0.0, 1.0);
    const float cos_light = dot(normal, -l);
    if (distance <= 1e-6 || cos_light <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    float falloff = 1.0;
    if (light.kind == light_spot) {  // the cone, toward p from the sphere's center
        const float cd = dot(light.direction.xyz, normalize(p - light.position.xyz));
        falloff = saturate((cd - light.cos_outer) / max(light.cos_inner - light.cos_outer, 1e-4));
        falloff *= falloff;
    }
    const float3 radiance = light.radiance.xyz * falloff / (pi * light.size * light.size);
    return radiance * cos_light / (distance * distance);
}

/// What any punctual light sends toward p: exactly for a point (punctual_irradiance), or one sample of its extent
/// divided by its density, using two random numbers only when it has a size.
float3 punctual_irradiance_sample(LightRecord light, float3 p, inout Rng rng, out float3 l, out float distance) {
    if (light.size <= 0.0) {
        return punctual_irradiance(light, p, l, distance);
    }
    float3 point;
    float3 normal;
    sample_light_extent(light, float2(rng.next(), rng.next()), point, normal);
    return light_extent_incoming(light, point, normal, p, l, distance) / light_extent_pdf(light);
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
    const float3 irradiance = punctual_irradiance_sample(veach_scene.lights[index], p, rng, l, distance);
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
