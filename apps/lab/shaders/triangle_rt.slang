// First light, ray traced: the same triangle as triangle.slang, found with an
// inline ray query against a TLAS. Rays go through pixel centers, unprojected
// with the primary camera's inverse view-projection, so both paths agree. With
// no camera it is the identity, and rays are orthographic through NDC. Layout
// must match TraceParams in apps/lab/src/triangle_feature.cpp (ADR 0027).

import veach.bindless;

struct Vertex {
    float4 position;
    float4 color;
};

struct Params {
    float4x4 inverse_view_projection;
    DescriptorHandle<RWTexture2D<float4>> output;
    DescriptorHandle<RaytracingAccelerationStructure> scene;
    DescriptorHandle<StructuredBuffer<Vertex>> vertices;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

static const float4 background = float4(0.05, 0.05, 0.08, 1.0);  // render::background_color

float3 unproject(float2 ndc, float depth) {
    const float4 p = mul(params.inverse_view_projection, float4(ndc, depth, 1.0));
    return p.xyz / p.w;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    const float2 ndc = float2((float(id.x) + 0.5) / float(params.size.x) * 2.0 - 1.0,
                              1.0 - (float(id.y) + 0.5) / float(params.size.y) * 2.0);
    // Reverse-Z: the near plane is depth 1. A second point at depth 0.5 gives the ray's direction.
    const float3 near_point = unproject(ndc, 1.0);
    RayDesc ray;
    ray.Origin = near_point;
    ray.Direction = normalize(unproject(ndc, 0.5) - near_point);
    ray.TMin = 0.0;
    ray.TMax = 10000.0;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(params.scene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {
        // Opaque triangles only: there are no candidates to evaluate.
    }

    float4 color = background;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        const float2 b = query.CommittedTriangleBarycentrics();
        const uint first = query.CommittedPrimitiveIndex() * 3;
        const float3 c0 = params.vertices[first].color.rgb;
        const float3 c1 = params.vertices[first + 1].color.rgb;
        const float3 c2 = params.vertices[first + 2].color.rgb;
        color = float4(c0 * (1.0 - b.x - b.y) + c1 * b.x + c2 * b.y, 1.0);
    }
    params.output[id.xy] = color;
}
