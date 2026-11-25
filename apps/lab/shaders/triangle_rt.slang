// First light, ray traced: the same triangle as triangle.slang, found with an
// inline ray query against a TLAS. Orthographic rays through pixel centers, in
// the NDC space the raster path draws in, so both images match. Layout must
// match TraceParams in apps/lab/src/triangle_feature.cpp (ADR 0024).

struct Vertex {
    float4 position;
    float4 color;
};

struct Params {
    DescriptorHandle<RWTexture2D<float4>> output;
    DescriptorHandle<RaytracingAccelerationStructure> scene;
    DescriptorHandle<StructuredBuffer<Vertex>> vertices;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

static const float4 background = float4(0.05, 0.05, 0.08, 1.0);  // render::background_color

[shader("compute")]
[numthreads(8, 8, 1)]
void trace(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    const float2 ndc = float2((float(id.x) + 0.5) / float(params.size.x) * 2.0 - 1.0,
                              1.0 - (float(id.y) + 0.5) / float(params.size.y) * 2.0);
    RayDesc ray;
    ray.Origin = float3(ndc, 1.0);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin = 0.0;
    ray.TMax = 4.0;

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
