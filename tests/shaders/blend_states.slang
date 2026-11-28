// Blend modes and depth bias (tests/rhi_texture_tests.cpp, docs/specs/rhi-textures/spec.md): one flat-colored
// rectangle per draw, at a depth that may slope along x. Layout must match BlendStateParams there (ADR 0027).

struct BlendStateParams {
    float4 color;
    float4 rect;  // x0, y0, x1, y1 in NDC
    float depth;  // NDC z at x = 0
    float slope;  // dz per NDC unit of x
    uint unused0;
    uint unused1;
};
[[vk::push_constant]] ConstantBuffer<BlendStateParams> params;

struct VsOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VsOut vs_main(uint vertex: SV_VertexID) {
    // Two counter-clockwise triangles: (0, 1, 2) and (2, 1, 3) over the corners (x0,y0), (x1,y0), (x0,y1), (x1,y1).
    const uint corners[6] = { 0, 1, 2, 2, 1, 3 };
    const uint c = corners[vertex % 6];
    const float x = (c & 1) != 0 ? params.rect.z : params.rect.x;
    const float y = (c & 2) != 0 ? params.rect.w : params.rect.y;
    VsOut o;
    o.position = float4(x, y, params.depth + params.slope * x, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut input) : SV_Target {
    return params.color;
}
