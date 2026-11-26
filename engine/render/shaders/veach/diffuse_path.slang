// Diffuse-only light transport (docs/specs/candela/spec.md, ADR 0051): what Candela's radiance cache holds. Every
// surface is its diffuse albedo / pi, paths bounce by cosine sampling, each vertex samples the lights (MIS against the
// next bounce for emissive triangles), and rays that leave the scene see the sky. The path tracer's mode 4 follows
// the same rules inside its own loop (one trace() call site there), with the same lambert_eval, cosine_direction
// and sample_light(lambert).

import veach.atmosphere;
import veach.bindless;
import veach.brdf;
import veach.lights;
import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c)
import veach.random;
import veach.scene;

/// A surface a diffuse path reached.
struct DiffuseVertex {
    float3 position;
    float3 geometric;  ///< geometric normal, facing where the ray came from
    float3 normal;     ///< shading normal, same side
    float3 albedo;     ///< base color x (1 - metallic)
    float3 emission;   ///< toward the ray, MIS-weighted against the previous vertex's light sample
    bool lit;          ///< the ray arrived on the side the shading normal faces: the vertex reflects light
};

/// The surface trace `t` hit, reached from `origin` along `direction` by a bounce sampled with density
/// `previous_pdf` (0: not by a bounce, its emission is unweighted).
DiffuseVertex diffuse_vertex(Trace t, float3 origin, float3 direction, float previous_pdf) {
    const TriangleHit hit = surface_at(t.instance, t.primitive, t.bary, origin);
    MaterialContext ctx = { hit.surface, hit.material };
    const SurfaceSample s = evaluateSurface(hit.material.kind, ctx);
    DiffuseVertex v;
    v.position = hit.surface.position;
    v.geometric = hit.surface.geometric_normal;
    v.normal = s.normal;
    v.albedo = s.base_color * (1.0 - s.metallic);
    v.lit = dot(v.normal, -direction) > 0.0;
    v.emission = float3(0.0, 0.0, 0.0);
    if (any(s.emission > 0.0) && (hit.front || (hit.material.flags & material_double_sided) != 0)) {
        float weight = 1.0;
        if (previous_pdf > 0.0 && (hit.material.flags & material_emissive) != 0) {
            const float light_pdf = emissive_light_pdf(hit.material, hit.area, t.t, dot(v.geometric, -direction));
            weight = power_heuristic(previous_pdf, light_pdf);
        }
        v.emission = s.emission * weight;
    }
    return v;
}

/// One light sample at a diffuse vertex: radiance x albedo / pi x cos / pdf, shadowed.
float3 diffuse_light(DiffuseVertex v, inout Rng rng) {
    Brdf b;
    b.diffuse = v.albedo;
    b.f0 = float3(0.0, 0.0, 0.0);
    b.alpha = 1.0;
    b.specular_probability = 0.0;
    return sample_light(v.position, v.geometric, frame_of(v.normal), b, float3(0.0, 0.0, 1.0), rng, true);
}

/// A cosine-distributed direction about `frame`'s normal, and its solid-angle density.
float3 cosine_direction(Frame frame, inout Rng rng, out float pdf) {
    const float2 u = float2(rng.next(), rng.next());
    const float r = sqrt(u.x);
    const float3 l = float3(r * cos(2.0 * pi * u.y), r * sin(2.0 * pi * u.y), sqrt(1.0 - u.x));
    pdf = l.z / pi;
    return frame.to_world(l);
}
