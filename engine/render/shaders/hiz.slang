// The HiZ pyramid of the real-time renderer's culling (docs/adr/0093-gpu-driven-core.md): mip 0 is half the depth
// buffer, each texel the farthest (smallest, reverse-Z) depth of the pixels it covers; each mip halves the one before.
// Sizes round down (rhi::mip_size), so the last texel of a row or column also takes the source texel left over when
// the source is odd: texel t of mip L covers pixels [t, t + 1) * 2^(L + 1), and the last one reaches the edge.
// One dispatch per mip, each reading the mip before (render::RenderGraph::mips). Layout matches render::HizParams.

import veach.bindless;

struct HizParams {
    DescriptorHandle<Texture2D<float>> source;    ///< the depth buffer (mip 0) or a one-mip view of the mip before
    DescriptorHandle<RWTexture2D<float>> target;  ///< one mip
    uint2 source_size;
    uint2 target_size;
};
[[vk::push_constant]] ConstantBuffer<HizParams> params;

[shader("compute")]
[numthreads(8, 8, 1)]
void hiz_reduce(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.target_size)) {
        return;
    }
    const uint2 first = id.xy * 2;
    const uint2 last = uint2(id.x + 1 == params.target_size.x ? params.source_size.x - 1 : first.x + 1,
                             id.y + 1 == params.target_size.y ? params.source_size.y - 1 : first.y + 1);
    float farthest = 1.0;
    for (uint y = first.y; y <= max(last.y, first.y); ++y) {
        for (uint x = first.x; x <= max(last.x, first.x); ++x) {
            farthest = min(farthest, params.source.Load(int3(int2(min(uint2(x, y), params.source_size - 1)), 0)));
        }
    }
    params.target[id.xy] = farthest;
}
