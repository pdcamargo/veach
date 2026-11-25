// RHI raster state tests (tests/rhi_tests.cpp, rt.AC10/AC11): one flat triangle covering the lower-left half of
// the target, at a given NDC depth, writing uint2(id, ~id) into an rg32_uint target. Layout must match
// RasterStateParams in tests/rhi_tests.cpp (ADR 0024).

struct Params {
    float depth;     // NDC z; reverse-Z: larger is nearer
    uint id;         // written as uint2(id, ~id)
    uint clockwise;  // 1: wind the triangle clockwise (a back face; glTF fronts are counter-clockwise)
    uint unused;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

struct VsOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VsOut vs_main(uint vertex : SV_VertexID) {
    // (-1,-1), (1,-1), (-1,1): counter-clockwise; swapping the last two winds it clockwise.
    uint corner = vertex % 3;
    if (params.clockwise != 0 && corner != 0) {
        corner = 3 - corner;
    }
    const float2 p = corner == 0 ? float2(-1.0, -1.0) : corner == 1 ? float2(1.0, -1.0) : float2(-1.0, 1.0);
    VsOut o;
    o.position = float4(p, params.depth, 1.0);
    return o;
}

[shader("fragment")]
uint2 fs_main(VsOut input) : SV_Target {
    return uint2(params.id, ~params.id);
}
