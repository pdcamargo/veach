// Spike SP-16 (VCH-092) MSAA tests (tests/sp16_msaa_tests.cpp): one white triangle over the lower-left half of the
// target, its color's alpha `coverage` (alpha to coverage reads it). Layout matches MsaaParams.

struct Params {
    float coverage;
    uint unused0;
    uint unused1;
    uint unused2;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

struct VsOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VsOut vs_main(uint vertex: SV_VertexID) {
    const float2 p = vertex == 0 ? float2(-1.0, -1.0) : vertex == 1 ? float2(1.0, -1.0) : float2(-1.0, 1.0);
    VsOut o;
    o.position = float4(p, 0.5, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut input) : SV_Target {
    return float4(1.0, 1.0, 1.0, params.coverage);
}
