// RHI indirect tests (tests/indirect_tests.cpp, rhi.AC2): one instance per survivor of indirect_cull.slang's `cull`,
// a quad inset by a pixel inside the item's 8x8 cell of a 64x64 r32_uint target, written as item + 1 (0 is the
// clear value). Layout matches IndirectDrawParams.

import veach.bindless;

struct Params {
    DescriptorHandle<StructuredBuffer<uint>> survivors;  ///< [0] the count, then the item indices
    uint indexed;  ///< 1: SV_VertexID is a quad corner (an index buffer's 0..3); 0: one of two triangles' 6 vertices
    uint unused;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

struct VsOut {
    float4 position : SV_Position;
    nointerpolation uint item : ITEM;
};

[shader("vertex")]
VsOut vs_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const uint item = params.survivors[1 + instance];
    // Corners 0 (0,0), 1 (1,0), 2 (0,1), 3 (1,1); the non-indexed quad is the index list 0 1 2 2 1 3.
    const uint triangles[6] = { 0, 1, 2, 2, 1, 3 };
    const uint corner = params.indexed != 0 ? vertex : triangles[vertex % 6];
    const float2 cell = float2(float(item % 8), float(item / 8)) * 8.0;
    const float2 pixel = cell + 1.0 + float2(float(corner & 1), float(corner >> 1)) * 6.0;
    VsOut o;
    o.position = float4(pixel.x / 32.0 - 1.0, 1.0 - pixel.y / 32.0, 0.5, 1.0);
    o.item = item;
    return o;
}

[shader("fragment")]
uint fs_main(VsOut input) : SV_Target {
    return input.item + 1;
}
