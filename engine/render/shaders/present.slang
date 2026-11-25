// Copies the scene color into the swapchain image with a fullscreen triangle.
// Parameter layout must match PresentParams in renderer.cpp (ADR 0024).

import veach.bindless;

struct Params {
    DescriptorHandle<Texture2D<float4>> source;
    DescriptorHandle<SamplerState> sampler;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_main(uint vertex : SV_VertexID) {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    VsOut o;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut input) : SV_Target {
    return params.source.Sample(params.sampler, input.uv);
}
