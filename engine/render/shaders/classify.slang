// Material binning for the real-time renderer's shading (docs/adr/0093-gpu-driven-core.md): one thread per pixel, a
// group per 8x8 tile. Each pixel's bin is 0 where no geometry shows (the sky) and k + 1 for material kind k; the tile is
// appended to the list of every bin it shows, and classify_args turns the counts into one indirect dispatch per bin over
// exactly its tiles (realtime_shade.slang). It also completes the motion vectors where no
// geometry is: the sky moves with the camera's rotation only. Layout matches render::ClassifyParams.

import veach.bindless;
import veach.material;
import veach.scene;

static const uint no_geometry = 0xFFFFFFFFu;
static const uint bin_header = 32;         ///< bin_tiles starts with a count per bin (at most 32 bins)
static const uint groups_per_row = 32768;  ///< as gpu_cull.slang: D3D12 caps a dispatch row at 65535 groups

struct ClassifyParams {
    float4x4 inverse_view_projection;
    float4x4 previous_view_projection;  ///< last frame's, in this frame's render space
    DescriptorHandle<Texture2D<uint2>> visibility;
    DescriptorHandle<RWTexture2D<float2>> motion;
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<RWStructuredBuffer<uint>> bin_args;   ///< 16-byte DispatchIndirectArgs per bin
    /// bin_header counts, then per bin tile_capacity tiles: x | y << 16 (zeroed by gpu_cull.slang's cull_reset)
    DescriptorHandle<RWStructuredBuffer<uint>> bin_tiles;
    uint2 size;
    uint bin_count;  ///< material kinds + 1
    uint tile_capacity;
};
[[vk::push_constant]] ConstantBuffer<ClassifyParams> params;

groupshared uint tile_bins;

[shader("compute")]
[numthreads(8, 8, 1)]
void classify(uint3 id: SV_DispatchThreadID, uint3 group: SV_GroupID, uint index: SV_GroupIndex) {
    if (index == 0) {
        tile_bins = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < params.size)) {
        const uint2 visible_id = params.visibility.Load(int3(id.xy, 0));
        uint bin = 0;
        if (visible_id.x != no_geometry) {
            const InstanceRecord inst = params.instances[visible_id.x];
            const uint slot = nonuniform(inst.meshlets)[(visible_id.y >> 7) * 8 + 4];
            bin = min(params.materials[inst.material_base + slot].kind + 1, params.bin_count - 1);
        } else {
            // A direction at infinity: last frame's camera saw it where its rotation puts it.
            const float2 uv = (float2(id.xy) + 0.5) / float2(params.size);
            const float4 near = mul(params.inverse_view_projection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0));
            const float4 mid = mul(params.inverse_view_projection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
            const float3 direction = mid.xyz / mid.w - near.xyz / near.w;
            const float4 clip = mul(params.previous_view_projection, float4(direction, 0.0));
            const float2 ndc = clip.xy / clip.w;
            params.motion[id.xy] = clip.w > 0.0 ? float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) - uv : float2(0.0, 0.0);
        }
        InterlockedOr(tile_bins, 1u << bin);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < params.bin_count && (tile_bins & (1u << index)) != 0) {
        uint slot;
        InterlockedAdd(params.bin_tiles[index], 1u, slot);
        params.bin_tiles[bin_header + index * params.tile_capacity + slot] = group.x | (group.y << 16);
    }
}

/// Each bin's dispatch: a group per tile, in rows of groups_per_row.
[shader("compute")]
[numthreads(32, 1, 1)]
void classify_args(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.bin_count) {
        return;
    }
    const uint tiles = params.bin_tiles[id.x];
    params.bin_args[id.x * 4 + 0] = min(tiles, groups_per_row);
    params.bin_args[id.x * 4 + 1] = max((tiles + groups_per_row - 1) / groups_per_row, 1u);
    params.bin_args[id.x * 4 + 2] = 1;
    params.bin_args[id.x * 4 + 3] = 0;
}
