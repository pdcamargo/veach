// Comparison samplers (PCF) and anisotropic filtering (tests/rhi_texture_tests.cpp, docs/specs/rhi-textures/spec.md).
// Layout must match SamplerStateParams there (ADR 0027).

import veach.bindless;

struct SamplerStateParams {
    DescriptorHandle<Texture2D<float>> depth;
    DescriptorHandle<SamplerComparisonState> compare;
    DescriptorHandle<Texture2D<float4>> mips;
    DescriptorHandle<SamplerState> sampler;
    DescriptorHandle<StructuredBuffer<float4>> queries;
    DescriptorHandle<RWStructuredBuffer<float4>> results;
    uint count;
    uint unused0;
    uint unused1;
    uint unused2;
};
[[vk::push_constant]] ConstantBuffer<SamplerStateParams> params;

/// Query: (u, v, reference depth, unused). Result x: the fraction of the 2x2 (or 1) texels that pass.
[shader("compute")]
[numthreads(64, 1, 1)]
void compare_depth(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.count) {
        return;
    }
    const float4 q = params.queries[id.x];
    params.results[id.x] = float4(params.depth.SampleCmpLevelZero(params.compare, q.xy, q.z), 0.0, 0.0, 0.0);
}

/// Query: (u, v, du/dx, dv/dy): a footprint stretched along u when du/dx > dv/dy.
[shader("compute")]
[numthreads(64, 1, 1)]
void sample_grad(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.count) {
        return;
    }
    const float4 q = params.queries[id.x];
    params.results[id.x] = params.mips.SampleGrad(params.sampler, q.xy, float2(q.z, 0.0), float2(0.0, q.w));
}
