// The sun shadows' culling pyramids (docs/adr/0096-sun-shadow-culling.md, shadows.cpp): two coarse grids over each
// cascade's map, a texel per 16 x 16 of it, each a pyramid whose texels of level L hold the minimum of the four below
// (reverse-Z: the farthest from the light), like the camera's HiZ. gpu_cull.slang tests a caster's footprint on them.
//
// - Receivers (receivers_mark): what this frame's pixels look up. Every pixel of the camera's depth that reads a
//   cascade (by view depth, blend bands included) marks the grid texels its lookup can reach (the PCF footprint, the
//   normal offset and a margin) with its depth from the light. A caster whose nearest point to the light is farther
//   than every receiver in its footprint casts no shadow anyone sees; a texel no receiver marked holds FLT_MAX and
//   keeps nothing. Exact for this frame: what it drops is never sampled by the shading.
// - Occluders (occluders_build): what the cascade's map already holds after its first phase of casters. A caster
//   whose nearest point to the light is farther than every depth in its footprint adds no texel (the light's view of
//   two-phase occlusion: the second phase tests the first phase's rejects against this frame's pyramid, the next
//   frame's first phase against this one).
//
// One buffer holds every pyramid: layer k of `layers` at k * layer_stride, receivers first, then occluders.
// Layout matches render::PyramidParams (shadows.cpp).

import veach.bindless;
import veach.shadows;  // ShadowFrame

struct PyramidParams {
    float4x4 inverse_view_projection;  ///< the camera's (reverse-Z)
    DescriptorHandle<Texture2D<float>> depth;
    DescriptorHandle<StructuredBuffer<ShadowFrame>> shadows;
    DescriptorHandle<RWStructuredBuffer<uint>> pyramids;   ///< every pyramid, levels one after the other
    DescriptorHandle<Texture2DArray<float>> map;           ///< occluders_build: the shadow maps
    uint2 size;                                            ///< the depth buffer's
    uint mask_size;                                        ///< texels per side of level 0 (a power of two)
    uint levels;                                           ///< log2(mask_size) + 1
    uint layer_stride;                                     ///< uints per pyramid
    uint count;                                            ///< receivers_clear: uints to clear
    uint level;                                            ///< pyramid_reduce: the level read
    float margin_texels;  ///< how far (shadow-map texels) a lookup reaches from its receiver
    float depth_margin;   ///< shadow-map texels the receiver's lookup point may lie farther from the light
    uint first_layer;     ///< pyramid_reduce, occluders_build: the first pyramid (z = 0) they write
    uint map_size;        ///< occluders_build: the map's texels per side
    uint receivers;       ///< occluders_build: 1 if pyramids [0, first_layer) hold this frame's receivers
};
[[vk::push_constant]] ConstantBuffer<PyramidParams> params;

static const uint empty = 0x7F7FFFFFu;  ///< asuint(FLT_MAX): no receiver
static const uint max_layers = max_shadowed_lights * max_shadow_cascades;
/// A tile marks its box over a layer as a whole when it spans at most this many texels per side; a wider one
/// (a depth edge inside its 8 x 8 pixels) has each pixel mark its own.
static const uint group_box_limit = 4;

/// Where level `level` starts in a layer: the sizes before it sum to (4 M^2 - 4 (M >> level)^2) / 3.
uint level_offset(uint size, uint level) {
    const uint s = size >> level;
    return (4u * size * size - 4u * s * s) / 3u;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void receivers_clear(uint3 id: SV_DispatchThreadID) {
    const uint i = id.y * 64u * 32768u + id.x;
    if (i < params.count) {
        params.pyramids[i] = empty;
    }
}

groupshared uint tile_far;   ///< asuint of the tile's smallest receiver depth (reverse-Z: the farthest)...
groupshared uint tile_near;  ///< ...and its largest; 0: the tile shows only sky
groupshared float3 tile_corners[8];
groupshared uint tile_fallback;  ///< a bit per layer whose box is too wide: each pixel marks its own

/// pyramids[i] = min(pyramids[i], value). Values only fall: a plain read that is already as low skips the atomic,
/// which the neighbouring groups that mark the same texel would otherwise queue on.
void lower(uint i, uint value) {
    if (params.pyramids[i] > value) {
        InterlockedMin(params.pyramids[i], value);
    }
}

/// Grid texels [lo, hi] (level 0, inclusive) and the depth from the light a set of receivers marks in a layer.
struct Mark {
    uint2 lo;
    uint2 hi;
    uint depth;  ///< asuint of a depth >= 0; empty if it marks nothing in this layer
};

/// The mark of receivers whose positions on the layer's map span [at_lo, at_hi] (UV) and whose smallest depth from
/// the light is `depth`: widened by how far their lookups reach, and lowered by how much farther from the light the
/// lookups' points may be. Nothing if the lookups fall outside the map.
Mark mark_of(uint layer, float2 at_lo, float2 at_hi, float depth, float reach) {
    Mark mark;
    mark.lo = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    mark.hi = uint2(0u, 0u);
    mark.depth = empty;
    const float m = float(params.mask_size);
    const float2 lo = (at_lo - reach) * m;
    const float2 hi = (at_hi + reach) * m;
    if (any(hi < float2(0.0, 0.0)) || any(lo >= float2(m, m))) {
        return mark;
    }
    const float4 scale = params.shadows[0].cascades[layer].scale;
    mark.lo = uint2(clamp(lo, float2(0.0, 0.0), float2(m - 1.0, m - 1.0)));
    mark.hi = uint2(clamp(hi, float2(0.0, 0.0), float2(m - 1.0, m - 1.0)));
    mark.depth = asuint(max(depth - params.depth_margin * scale.x / scale.y, 0.0));
    return mark;
}

void write_mark(uint layer, Mark mark) {
    const uint base = layer * params.layer_stride;
    for (uint y = mark.lo.y; y <= mark.hi.y; ++y) {
        for (uint x = mark.lo.x; x <= mark.hi.x; ++x) {
            lower(base + y * params.mask_size + x, mark.depth);
        }
    }
}

/// Whether cascade c serves view depths overlapping [z_lo, z_hi]: the slice light_shadow picks it in, from where the
/// one before starts blending into it, with a margin for the shading's own reconstruction of the position.
bool serves(uint c, float z_lo, float z_hi) {
    const float4 splits = params.shadows[0].splits;
    const float near = c == 0 ? 0.0 : splits[c - 1] * (1.0 - params.shadows[0].blend);
    return z_hi >= near * 0.99 && z_lo <= splits[c] * 1.01;
}

float3 render_position(float2 pixel, float depth) {
    const float2 uv = pixel / float2(params.size);
    const float4 h = mul(params.inverse_view_projection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return h.xyz / h.w;
}

/// An 8 x 8 tile's receivers lie in the piece of the view frustum between its pixel centers' rays and its depth
/// range: its eight corners bound them, on every cascade's map (orthographic). One thread per layer marks that box;
/// where it is too wide (a depth edge in the tile), every pixel marks its own.
[shader("compute")]
[numthreads(8, 8, 1)]
void receivers_mark(uint3 id: SV_DispatchThreadID, uint3 group: SV_GroupID, uint index: SV_GroupIndex) {
    if (index == 0) {
        tile_far = empty;
        tile_near = 0;
        tile_fallback = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint light_count = params.shadows[0].light_count;
    const uint cascade_count = params.shadows[0].cascade_count;
    const uint layers = light_count * cascade_count;
    const float d = all(id.xy < params.size) ? params.depth.Load(int3(int2(id.xy), 0)) : 0.0;
    const bool receiver = d > 0.0 && layers > 0;  // depth 0: the sky, which looks nothing up
    const uint far = WaveActiveMin(receiver ? asuint(d) : empty);
    const uint near = WaveActiveMax(receiver ? asuint(d) : 0u);
    if (WaveIsFirstLane() && near != 0) {
        InterlockedMin(tile_far, far);
        InterlockedMax(tile_near, near);
    }
    GroupMemoryBarrierWithGroupSync();
    if (tile_near == 0) {
        return;  // the whole group: no receiver
    }
    if (index < 8) {
        const uint2 first = group.xy * 8;
        const uint2 last = min(first + 7, params.size - 1);
        const float2 pixel = float2((index & 1) != 0 ? last.x : first.x, (index & 2) != 0 ? last.y : first.y) + 0.5;
        tile_corners[index] = render_position(pixel, asfloat((index & 4) != 0 ? tile_near : tile_far));
    }
    GroupMemoryBarrierWithGroupSync();
    const float4 axis = params.shadows[0].view_axis;
    const float reach = params.margin_texels / params.shadows[0].resolution;
    if (index < layers) {
        const uint layer = index;
        float z_lo = 3.4e38;
        float z_hi = -3.4e38;
        for (uint i = 0; i < 8; ++i) {
            const float z = dot(axis.xyz, tile_corners[i]) + axis.w;
            z_lo = min(z_lo, z);
            z_hi = max(z_hi, z);
        }
        if (serves(layer % cascade_count, z_lo, z_hi)) {
            const float4x4 clip_from_render = params.shadows[0].cascades[layer].clip_from_render;
            float2 at_lo = float2(3.4e38, 3.4e38);
            float2 at_hi = float2(-3.4e38, -3.4e38);
            float depth = 3.4e38;
            for (uint i = 0; i < 8; ++i) {
                const float4 clip = mul(clip_from_render, float4(tile_corners[i], 1.0));
                const float2 at = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
                at_lo = min(at_lo, at);
                at_hi = max(at_hi, at);
                depth = min(depth, clip.z);
            }
            const Mark mark = mark_of(layer, at_lo, at_hi, depth, reach);
            if (mark.depth != empty) {
                if (all(mark.hi - mark.lo < uint2(group_box_limit, group_box_limit))) {
                    write_mark(layer, mark);
                } else {
                    InterlockedOr(tile_fallback, 1u << layer);
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (tile_fallback == 0 || !receiver) {
        return;
    }
    const float3 p = render_position(float2(id.xy) + 0.5, d);
    const float z = dot(axis.xyz, p) + axis.w;
    for (uint layer = 0; layer < layers; ++layer) {
        if ((tile_fallback & (1u << layer)) == 0 || !serves(layer % cascade_count, z, z)) {
            continue;
        }
        const float4 clip = mul(params.shadows[0].cascades[layer].clip_from_render, float4(p, 1.0));
        const float2 at = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
        const Mark mark = mark_of(layer, at, at, clip.z, reach);
        if (mark.depth != empty) {
            write_mark(layer, mark);
        }
    }
}

groupshared uint reduce_tile[64];

/// Levels level + 1 to level + 4 of pyramids first_layer + z from `level`: each group covers 8 x 8 texels of
/// level + 1.
[shader("compute")]
[numthreads(8, 8, 1)]
void pyramid_reduce(uint3 id: SV_DispatchThreadID, uint3 thread: SV_GroupThreadID, uint index: SV_GroupIndex) {
    const uint m = params.mask_size;
    const uint base = (params.first_layer + id.z) * params.layer_stride;
    const uint source_size = m >> params.level;
    const uint source = base + level_offset(m, params.level);
    uint value = empty;
    if (all(id.xy < uint2(source_size / 2, source_size / 2))) {
        const uint2 s = id.xy * 2;
        value = min(min(params.pyramids[source + s.y * source_size + s.x],
                        params.pyramids[source + s.y * source_size + s.x + 1]),
                    min(params.pyramids[source + (s.y + 1) * source_size + s.x],
                        params.pyramids[source + (s.y + 1) * source_size + s.x + 1]));
        params.pyramids[base + level_offset(m, params.level + 1) + id.y * (source_size / 2) + id.x] = value;
    }
    reduce_tile[index] = value;
    GroupMemoryBarrierWithGroupSync();
    // Three more levels inside the group's tile: 4 x 4, 2 x 2, 1.
    uint2 tile = id.xy - thread.xy;  // the group's first texel of level + 1
    for (uint step = 1; step <= 3; ++step) {
        const uint level = params.level + 1 + step;
        const uint width = 8u >> step;  // texels of this level in the group's tile, per side
        uint reduced = empty;
        if (all(thread.xy < uint2(width, width))) {
            const uint2 c = thread.xy * 2;
            const uint stride = 8u;
            reduced = min(min(reduce_tile[c.y * stride + c.x], reduce_tile[c.y * stride + c.x + 1]),
                          min(reduce_tile[(c.y + 1) * stride + c.x], reduce_tile[(c.y + 1) * stride + c.x + 1]));
        }
        GroupMemoryBarrierWithGroupSync();
        if (all(thread.xy < uint2(width, width))) {
            reduce_tile[thread.y * 8 + thread.x] = reduced;
            const uint size = m >> level;
            const uint2 at = (tile >> step) + thread.xy;
            if (level < params.levels && all(at < uint2(size, size))) {
                params.pyramids[base + level_offset(m, level) + at.y * size + at.x] = reduced;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

groupshared uint build_wave[1];

/// Level 0 of the occluder pyramid of map layer z (pyramid first_layer + z): each group the smallest depth of one
/// grid texel's map texels, [g * map_size / mask_size, (g + 1) * map_size / mask_size) per axis. Where this frame's
/// receivers (pyramid z, when `receivers` is set) mark nothing, no lookup reads the map and anything there may be
/// dropped: the texel is FLT_MAX, and the map is not read.
[shader("compute")]
[numthreads(8, 8, 1)]
void occluders_build(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID, uint index: SV_GroupIndex) {
    const uint cell = group.y * params.mask_size + group.x;
    const uint target = (params.first_layer + group.z) * params.layer_stride + cell;
    if (params.receivers != 0 && params.pyramids[group.z * params.layer_stride + cell] == empty) {
        if (index == 0) {
            params.pyramids[target] = empty;
        }
        return;
    }
    const uint2 lo = group.xy * params.map_size / params.mask_size;
    const uint2 hi = (group.xy + 1) * params.map_size / params.mask_size;
    float farthest = 3.4e38;
    for (uint y = lo.y + thread.y; y < hi.y; y += 8) {
        for (uint x = lo.x + thread.x; x < hi.x; x += 8) {
            farthest = min(farthest, params.map.Load(int4(int(x), int(y), int(group.z), 0)));
        }
    }
    const float wave = WaveActiveMin(farthest);
    if (index == 0) {
        build_wave[0] = empty;
    }
    GroupMemoryBarrierWithGroupSync();
    if (WaveIsFirstLane()) {
        InterlockedMin(build_wave[0], asuint(max(wave, 0.0)));
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        params.pyramids[target] = build_wave[0];
    }
}
