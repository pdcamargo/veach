// The scene on the GPU, as both renderers read it (ADR 0034, ADR 0037): instance, material, light and emitter
// records, surface reconstruction from barycentrics, alpha coverage and ray tracing with alpha continuation
// (ADR 0035). Entry points fill `veach_scene` from their parameters before calling anything here. Layouts
// mirror render::InstanceRecord, LightRecord and EmissiveTriangle (path_tracer.cpp).

import veach.bindless;
import veach.material;
import veach.materials_generated;  // evaluateSurface(kind, c), evaluateAlpha(kind, c)
import veach.random;

/// Mirrors render::InstanceRecord: 160 bytes per instance.
struct InstanceRecord {
    DescriptorHandle<StructuredBuffer<float>> vertices;  ///< 12 floats per vertex: position, normal, tangent, uv
    DescriptorHandle<StructuredBuffer<uint>> indices;
    DescriptorHandle<StructuredBuffer<uint>> slots;  ///< material slot per triangle
    uint material_base;                                 ///< slot 0's index in the material buffer
    uint flags;                                         ///< instance_has_alpha
    float4 object_to_world[3];  ///< rows of a 3x4 matrix
    float4 normal_to_world[3];  ///< rows of the inverse transpose
    // Meshlets (ADR 0037): 8 uints per meshlet (offsets, counts, slot, lod, parent, flags), a mesh vertex index
    // per meshlet vertex, 3 local vertex indices packed per triangle.
    DescriptorHandle<StructuredBuffer<uint>> meshlets;
    DescriptorHandle<StructuredBuffer<uint>> meshlet_vertices;
    DescriptorHandle<StructuredBuffer<uint>> meshlet_triangles;
    uint meshlet_count;
    uint pad;
};

static const uint instance_has_alpha = 1;  ///< some slot is alpha-masked or blended

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

/// What the scene functions read. Copied from the entry point's parameters.
struct SceneData {
    DescriptorHandle<RaytracingAccelerationStructure> tlas;
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<StructuredBuffer<LightRecord>> lights;
    DescriptorHandle<StructuredBuffer<EmissiveTriangle>> emissive;
    uint light_count;
    uint emissive_count;
    float emissive_total_weight;  ///< sum of area x weight over every emissive triangle
};
static SceneData veach_scene;  // filled by the entry point before any use

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

// An instance's buffers differ from hit to hit across a wave, so every access goes through `nonuniform`.
float vertex_float(InstanceRecord inst, uint i) {
    return nonuniform(inst.vertices)[i];
}
uint vertex_index(InstanceRecord inst, uint i) {
    return nonuniform(inst.indices)[i];
}
uint slot_of(InstanceRecord inst, uint primitive) {
    return nonuniform(inst.slots)[primitive];
}

Corner fetch(InstanceRecord inst, uint vertex) {
    const uint b = vertex * 12;
    Corner c;
    c.position = float3(vertex_float(inst, b), vertex_float(inst, b + 1), vertex_float(inst, b + 2));
    c.normal = float3(vertex_float(inst, b + 3), vertex_float(inst, b + 4), vertex_float(inst, b + 5));
    c.tangent = float4(vertex_float(inst, b + 6), vertex_float(inst, b + 7), vertex_float(inst, b + 8), vertex_float(inst, b + 9));
    c.uv = float2(vertex_float(inst, b + 10), vertex_float(inst, b + 11));
    return c;
}

struct TriangleHit {
    SurfaceInput surface;
    MaterialRecord material;
    float area;  ///< world space
    bool front;  ///< the ray hit the side the vertex winding faces
};

/// A triangle by its mesh vertex indices and material slot: what a ray hit (index buffer) or a visibility-buffer
/// pixel (meshlet) names.
struct TriangleRef {
    uint3 vertices;
    uint slot;
};

TriangleRef triangle_of(InstanceRecord inst, uint primitive) {
    TriangleRef t;
    t.vertices = uint3(vertex_index(inst, primitive * 3), vertex_index(inst, primitive * 3 + 1),
                       vertex_index(inst, primitive * 3 + 2));
    t.slot = slot_of(inst, primitive);
    return t;
}

TriangleRef meshlet_triangle(InstanceRecord inst, uint meshlet, uint triangle) {
    const uint vertex_offset = nonuniform(inst.meshlets)[meshlet * 8];
    const uint triangle_offset = nonuniform(inst.meshlets)[meshlet * 8 + 1];
    const uint packed = nonuniform(inst.meshlet_triangles)[triangle_offset + triangle];
    TriangleRef t;
    t.vertices = uint3(nonuniform(inst.meshlet_vertices)[vertex_offset + (packed & 0xFFu)],
                       nonuniform(inst.meshlet_vertices)[vertex_offset + ((packed >> 8) & 0xFFu)],
                       nonuniform(inst.meshlet_vertices)[vertex_offset + ((packed >> 16) & 0xFFu)]);
    t.slot = nonuniform(inst.meshlets)[meshlet * 8 + 4];
    return t;
}

/// Object-space positions of a triangle's corners.
float3x3 triangle_positions(InstanceRecord inst, TriangleRef t) {
    return float3x3(fetch(inst, t.vertices.x).position, fetch(inst, t.vertices.y).position,
                    fetch(inst, t.vertices.z).position);
}

TriangleHit surface_at(uint instance_index, uint primitive, float2 bary, float3 view_origin) {
    const InstanceRecord inst = veach_scene.instances[instance_index];
    return surface_of(inst, triangle_of(inst, primitive), bary, view_origin);
}

/// The surface at barycentrics (u, v) of a triangle, seen from `view_origin`. UV gradients are zero (mip 0); the
/// real-time renderer sets them from neighboring pixels.
TriangleHit surface_of(InstanceRecord inst, TriangleRef tri, float2 bary, float3 view_origin) {
    const Corner c0 = fetch(inst, tri.vertices.x);
    const Corner c1 = fetch(inst, tri.vertices.y);
    const Corner c2 = fetch(inst, tri.vertices.z);
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
    hit.surface.uv_dx = float2(0.0, 0.0);
    hit.surface.uv_dy = float2(0.0, 0.0);
    // Every surface is two-sided for the path tracer; `front` remembers which side was hit (emission is one-sided).
    hit.front = dot(geometric, hit.surface.view) >= 0.0;
    if (!hit.front) {
        geometric = -geometric;
        normal = -normal;
    }
    hit.surface.geometric_normal = geometric;
    hit.surface.normal = normal;
    hit.material = veach_scene.materials[inst.material_base + tri.slot];
    return hit;
}

/// Alpha coverage at a hit: mask tests the cutoff, blend is stochastic (ADR 0034). Minimal on purpose (the
/// material and one UV): it runs for every masked or blended surface a ray meets.
bool covered(uint instance_index, uint primitive, float2 bary, inout Rng rng) {
    const InstanceRecord inst = veach_scene.instances[instance_index];
    const MaterialRecord material = veach_scene.materials[inst.material_base + slot_of(inst, primitive)];
    if ((material.flags & (material_alpha_mask | material_alpha_blend)) == 0) {
        return true;
    }
    const float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float2 uv = float2(0.0, 0.0);
    for (uint k = 0; k < 3; ++k) {
        const uint b = vertex_index(inst, primitive * 3 + k) * 12 + 10;
        uv += float2(vertex_float(inst, b), vertex_float(inst, b + 1)) * w[k];
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

/// The closest covered hit. Everything is traced as opaque; when the closest hit is a transparent texel of a
/// masked or blended surface, the same ray continues just past it. That is deterministic on every backend,
/// unlike committing non-opaque candidates, whose results varied run to run on Metal (and whose inlined
/// candidate loop crashed Metal's shader compiler).
Trace trace(float3 origin, float3 direction, float tmax, inout Rng rng) {
    Trace result;
    result.hit = false;
    result.instance = 0;
    result.primitive = 0;
    result.bary = float2(0.0, 0.0);
    result.t = 0.0;
    float tmin = 0.0;
    // One query object, traced again per layer: a query allocated inside the loop is a new allocation per
    // iteration in DXIL.
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    for (uint layer = 0; layer < 16; ++layer) {
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = direction;
        ray.TMin = tmin;
        ray.TMax = tmax;
        query.TraceRayInline(veach_scene.tlas, RAY_FLAG_NONE, 0xFF, ray);
        while (query.Proceed()) {
        }
        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            return result;
        }
        const uint instance = query.CommittedInstanceIndex();
        const uint primitive = query.CommittedPrimitiveIndex();
        const float2 bary = query.CommittedTriangleBarycentrics();
        const float t = query.CommittedRayT();
        // Only instances with a masked or blended slot pay for the coverage test: an optimization. (WARP's
        // device removal blamed on this test came from handles used without `nonuniform`, ADR 0039.)
        const bool opaque = (veach_scene.instances[instance].flags & instance_has_alpha) == 0;
        if (opaque || covered(instance, primitive, bary, rng)) {
            result.hit = true;
            result.instance = instance;
            result.primitive = primitive;
            result.bary = bary;
            result.t = t;
            return result;
        }
        tmin = t * (1.0 + 1e-5) + 1e-5;
    }
    return result;
}

/// Shadow rays start a little off the surface along the geometric normal, on the side they leave from.
float3 offset_origin(float3 p, float3 geometric, float3 direction) {
    return p + geometric * (dot(geometric, direction) >= 0.0 ? 1e-3 : -1e-3);
}

bool visible(float3 from, float3 to_direction, float distance, inout Rng rng) {
    return !trace(from, to_direction, distance, rng).hit;
}
