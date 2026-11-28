// The lab's blend-mode demo (src/blend_demo.cpp, docs/specs/rhi-textures/spec.md): flat-colored rectangles.
// Layout must match BlendDemoParams there (ADR 0027).

struct BlendDemoParams {
    float4 color;
    float4 rect;  // x0, y0, x1, y1 in NDC
};
[[vk::push_constant]] ConstantBuffer<BlendDemoParams> params;

struct VsOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VsOut vs_main(uint vertex: SV_VertexID) {
    // Two counter-clockwise triangles over the corners (x0,y0), (x1,y0), (x0,y1), (x1,y1).
    const uint corners[6] = { 0, 1, 2, 2, 1, 3 };
    const uint c = corners[vertex % 6];
    VsOut o;
    o.position = float4((c & 1) != 0 ? params.rect.z : params.rect.x, (c & 2) != 0 ? params.rect.w : params.rect.y,
                        0.5, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut input) : SV_Target {
    return params.color;
}
