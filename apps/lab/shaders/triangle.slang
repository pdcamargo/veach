// First light, rasterized: one triangle with per-vertex colors, vertices pulled
// through a bindless handle. Layout must match RasterParams in
// apps/lab/src/triangle_feature.cpp (ADR 0024).

struct Vertex {
    float4 position;  // xyz in NDC (z = 0.5), w unused
    float4 color;
};

struct Params {
    DescriptorHandle<StructuredBuffer<Vertex>> vertices;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

struct VsOut {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VsOut vs_main(uint vertex : SV_VertexID) {
    const Vertex v = params.vertices[vertex];
    VsOut o;
    o.position = float4(v.position.xyz, 1.0);
    o.color = v.color.rgb;
    return o;
}

[shader("fragment")]
float4 fs_main(VsOut input) : SV_Target {
    return float4(input.color, 1.0);
}
