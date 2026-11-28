// atmosphere.AC1: the shader's ray-sphere helpers (sphere_span, sphere_blocks in veach/atmosphere.slang) at chosen
// rays, read back and compared with their CPU twins and a double-precision reference (tests/atmosphere_tests.cpp).
// Mirrors SphereProbeParams there.

import veach.atmosphere;
import veach.bindless;

struct SphereProbeParams {
    /// Two per query: (origin.xyz, c) and (direction.xyz, radius). Spans use c; the shadow test uses the radius.
    DescriptorHandle<StructuredBuffer<float4>> queries;
    /// One per query: x = t0, y = t1, z = 1 if the line meets the sphere, w = 1 if sphere_blocks.
    DescriptorHandle<RWStructuredBuffer<float4>> results;
    uint count;
};
[[vk::push_constant]] ConstantBuffer<SphereProbeParams> probe;

[shader("compute")]
[numthreads(64, 1, 1)]
void sphere_probe(uint3 id: SV_DispatchThreadID) {
    if (id.x >= probe.count) {
        return;
    }
    const float4 a = probe.queries[2 * id.x];
    const float4 b = probe.queries[2 * id.x + 1];
    float t0;
    float t1;
    const bool hit = sphere_span(a.xyz, b.xyz, a.w, t0, t1);
    const bool blocks = sphere_blocks(a.xyz, b.xyz, b.w);
    probe.results[id.x] = float4(t0, t1, hit ? 1.0 : 0.0, blocks ? 1.0 : 0.0);
}
