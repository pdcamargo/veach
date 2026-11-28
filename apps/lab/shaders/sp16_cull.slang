// Spike SP-16 (VCH-092), THROWAWAY: GPU culling shared by both renderers. `cull` tests each instance's bounding
// sphere against the frustum, its model's draw distance and last frame's HiZ (the farthest depth per texel, reverse-Z),
// and appends survivors to each of the model's parts' lists. `write_args` turns the counts into one indexed indirect
// draw per part (ADR 0089: compacted lists, instance_count from the GPU) and zeroes them. `hiz_first` and `hiz_down`
// build the pyramid from this frame's depth for the next frame. Layout matches CullParams in sp16_spike.cpp.

import veach.bindless;
import sp16.common;

struct CullParams {
    DescriptorHandle<StructuredBuffer<FrameData>> frame;
    DescriptorHandle<StructuredBuffer<Instance>> instances;
    DescriptorHandle<StructuredBuffer<Model>> models;
    DescriptorHandle<StructuredBuffer<Part>> parts;
    DescriptorHandle<RWStructuredBuffer<uint>> counts;  ///< one per part
    DescriptorHandle<RWStructuredBuffer<uint>> lists;
    DescriptorHandle<RWStructuredBuffer<uint>> args;  ///< DrawIndexedIndirectArgs per part, as uints
    DescriptorHandle<RWStructuredBuffer<float>> hiz;  ///< every level, offsets in FrameData::hiz_offsets
    DescriptorHandle<Texture2D<float>> depth;         ///< hiz_first's source
    uint instance_count;
    uint part_count;
    uint2 src_size;  ///< hiz: the level read
    uint2 dst_size;  ///< hiz: the level written
    uint src_offset;
    uint dst_offset;
    uint flags;  ///< 1: occlusion culling on
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<CullParams> params;

uint level_offset(FrameData f, uint level) {
    return f.hiz_offsets[level / 4][level % 4];
}

/// True if the sphere is hidden behind last frame's depth: its nearest depth is farther than the farthest depth
/// of every HiZ texel its screen rectangle touches.
bool occluded(FrameData f, float3 center, float radius) {
    if ((params.flags & 1u) == 0 || f.hiz.w == 0) {
        return false;
    }
    float2 lo = float2(1.0, 1.0);
    float2 hi = float2(0.0, 0.0);
    float nearest = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        const float3 corner = center + radius * float3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0,
                                                       (i & 4) != 0 ? 1.0 : -1.0);
        const float4 clip = mul_columns(f.previous_view_projection, float4(corner, 1.0));
        if (clip.w <= f.sun_direction.w) {
            return false;  // reaches behind the near plane: visible
        }
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        lo = min(lo, uv);
        hi = max(hi, uv);
        nearest = max(nearest, ndc.z);  // reverse-Z: nearer is larger
    }
    lo = saturate(lo);
    hi = saturate(hi);
    if (any(hi <= lo)) {
        return false;  // off screen (the frustum test decides) or degenerate
    }
    const float2 size0 = float2(f.hiz.xy);
    const float2 extent = (hi - lo) * size0;
    const uint level = min(uint(ceil(log2(max(max(extent.x, extent.y), 1.0)))), f.hiz.z - 1);
    const uint2 size = (f.hiz.xy + (1u << level) - 1) >> level;  // each level is ceil(previous / 2)
    const uint2 a = min(uint2(lo * float2(size0) / float(1u << level)), size - 1);
    const uint2 b = min(uint2(hi * float2(size0) / float(1u << level)), size - 1);
    const uint base = level_offset(f, level);
    float farthest = 1.0;
    for (uint y = a.y; y <= b.y; ++y) {
        for (uint x = a.x; x <= b.x; ++x) {
            farthest = min(farthest, params.hiz[base + y * size.x + x]);
        }
    }
    return nearest < farthest;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cull(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.instance_count) {
        return;
    }
    const FrameData f = params.frame[0];
    const Instance inst = params.instances[id.x];
    const float3 center = inst.sphere.xyz;
    const float radius = inst.sphere.w;
    for (uint i = 0; i < 5; ++i) {
        if (dot(f.planes[i].xyz, center) + f.planes[i].w < -radius) {
            return;
        }
    }
    const Model model = params.models[inst.model];
    if (length(center - f.camera.xyz) - radius > model.max_distance) {
        return;
    }
    if (occluded(f, center, radius)) {
        return;
    }
    for (uint p = 0; p < model.part_count; ++p) {
        const uint part_index = model.first_part + p;
        const Part part = params.parts[part_index];
        uint slot;
        InterlockedAdd(params.counts[part_index], 1u, slot);
        params.lists[part.list_offset + slot] = id.x;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void write_args(uint3 id: SV_DispatchThreadID) {
    if (id.x >= params.part_count) {
        return;
    }
    const Part part = params.parts[id.x];
    const uint base = id.x * 5;
    params.args[base + 0] = part.index_count;
    params.args[base + 1] = params.counts[id.x];
    params.args[base + 2] = part.first_index;
    params.args[base + 3] = 0;  // vertex_offset: indices are absolute
    params.args[base + 4] = 0;  // first_instance stays 0 (ADR 0089)
    params.counts[id.x] = 0;
}

/// Level 0: the farthest (smallest, reverse-Z) of each 2x2 block of the depth buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void hiz_first(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.dst_size)) {
        return;
    }
    const uint2 s = id.xy * 2;
    const uint2 m = params.src_size - 1;
    const float d = min(min(params.depth.Load(int3(min(s, m), 0)), params.depth.Load(int3(min(s + uint2(1, 0), m), 0))),
                        min(params.depth.Load(int3(min(s + uint2(0, 1), m), 0)),
                            params.depth.Load(int3(min(s + uint2(1, 1), m), 0))));
    params.hiz[params.dst_offset + id.y * params.dst_size.x + id.x] = d;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void hiz_down(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.dst_size)) {
        return;
    }
    const uint2 s = id.xy * 2;
    const uint2 m = params.src_size - 1;
    const uint w = params.src_size.x;
    const uint o = params.src_offset;
    const uint2 p00 = min(s, m);
    const uint2 p10 = min(s + uint2(1, 0), m);
    const uint2 p01 = min(s + uint2(0, 1), m);
    const uint2 p11 = min(s + uint2(1, 1), m);
    const float d = min(min(params.hiz[o + p00.y * w + p00.x], params.hiz[o + p10.y * w + p10.x]),
                        min(params.hiz[o + p01.y * w + p01.x], params.hiz[o + p11.y * w + p11.x]));
    params.hiz[params.dst_offset + id.y * params.dst_size.x + id.x] = d;
}
