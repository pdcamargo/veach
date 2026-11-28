// Temporal anti-aliasing (ADR 0061, docs/specs/temporal-aa/spec.md): a resolve between the shade pass and the display
// transform, on pre-exposed radiance, then a light sharpening of the displayed image where it moves. Each pixel of this
// frame (rendered with a sub-pixel jitter) is blended with its history, last frame's result reprojected by the motion
// vectors:
//
// - the motion of the nearest surface in the 3x3 neighborhood, so silhouettes move with what is in front;
// - the history read with a 5-tap Catmull-Rom filter (sharper than bilinear), rescaled to this frame's exposure;
// - rejected where it shows another surface: the history keeps each pixel's surface id (InstanceRecord::id, the sky
//   its own) and linear depth; a pixel whose surface is in none of the 3x3 history texels around where it was (by id,
//   or by the depth last frame's camera would have seen it at) is disoccluded and starts over. Jitter moves every edge
//   by up to a pixel between frames, so the surface an edge pixel shows alternates: a neighbor's match keeps it;
// - clipped toward this frame's 3x3 neighborhood in YCoCg (variance clipping inside the min/max box), so what the
//   reprojection got wrong cannot ghost;
// - blended in Karis's luminance-compressed space, c / (1 + luma), so one bright sample cannot dominate. The weight of
//   this frame is 1 / samples while the history is young (a fresh pixel converges as a plain average), and at least
//   min_weight still, rising to moving_weight for a pixel that moves moving_pixels a frame; edge_weight where its
//   surface was found only beside where it was while moving (a trail behind something that moved in front: the
//   neighborhood of a foliage edge holds both colors, so clipping alone cannot catch it).
//
// Alpha is 1 + the share of the pixel that shows the sky in front of the space backdrop (the shade pass writes 2 there
// and 1 elsewhere); it is blended and clipped like the color, and the backdrop is drawn after these passes on that
// share (backdrop_composite.slang), so stars are never filtered in time. Radiance is clamped to the fp16 maximum (ADR
// 0082). Layout matches render::TaaParams (realtime_renderer.cpp).

import veach.bindless;
import veach.scene;

static const uint taa_history_valid = 1u;     ///< flags: the history is last frame's, at this size and camera
static const uint taa_auto_exposure = 2u;     ///< flags: the exposure ratio is state[0].y / state[0].z (exposure.slang)
static const uint no_geometry = 0xFFFFFFFFu;  ///< the visibility buffer's clear value
static const uint id_mask = 0x00FFFFFFu;      ///< meta.x: the surface id in the low 24 bits, samples above
static const uint sky_id = 0x00FFFFFFu;       ///< the sky's surface id (render::surface_id never makes it)
static const uint max_samples = 255u;
static const float fp16_max = 65504.0;
static const float sky_depth = 3.0e38;  ///< the linear depth the history keeps for the sky

struct TaaParams {
    /// From this frame's clip space (jittered: the depth buffer's) to last frame's, unjittered, in this frame's render
    /// space, divided by this frame's w: where last frame's camera would have seen each pixel's surface had it not
    /// moved.
    float4x4 reprojection;
    DescriptorHandle<Texture2D<float4>> radiance;   ///< this frame, pre-exposed; alpha 1 + backdrop share
    DescriptorHandle<Texture2D<float2>> motion;     ///< previous minus current position, UV, without jitter
    DescriptorHandle<Texture2D<float>> depth;       ///< reverse-Z, infinite far plane
    DescriptorHandle<Texture2D<uint2>> visibility;  ///< instance, meshlet << 7 | triangle
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<Texture2D<float4>> history;      ///< last frame's result, at last frame's pre-exposure
    DescriptorHandle<Texture2D<uint2>> history_meta;  ///< surface id | samples << 24, linear depth bits
    DescriptorHandle<SamplerState> linear_clamp;
    DescriptorHandle<RWTexture2D<float4>> history_out;  ///< next frame's history
    DescriptorHandle<RWTexture2D<uint2>> meta_out;
    DescriptorHandle<RWTexture2D<float4>> resolved;             ///< this pass's result
    DescriptorHandle<StructuredBuffer<float4>> exposure_state;  ///< with taa_auto_exposure (spike SP-14)
    uint2 size;
    float near_plane;
    float exposure_ratio;  ///< without taa_auto_exposure: this frame's exposure over the history's
    uint flags;
    float min_weight;     ///< the least weight of this frame in the blend, for a still pixel
    float moving_weight;  ///< ...and for one that moves `moving_pixels` or more a frame
    float moving_pixels;
    float clip_gamma;   ///< the variance clip's half-width, in standard deviations
    float edge_weight;  ///< the least weight where the surface was found only beside where it was, while moving
    float sharpness;    ///< taa_sharpen: the most it sharpens, for pixels that move a pixel a frame or more
    float pad0;
};
[[vk::push_constant]] ConstantBuffer<TaaParams> params;

float luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/// Karis's reversible tone map: c / (1 + luma). Blending and clipping here weighs each sample by 1 / (1 + luma).
float3 compress(float3 c) {
    return c / (1.0 + luma(c));
}

float3 expand(float3 c) {
    return c / max(1.0 - luma(c), 1.0 / fp16_max);
}

float3 to_ycocg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 from_ycocg(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float3 compressed(float3 c) {
    return to_ycocg(compress(max(c, float3(0.0, 0.0, 0.0))));
}

/// `h` moved toward the box's center until it is inside the box (Playdead's clip, not a per-axis clamp: the color
/// keeps its hue).
float3 clip_to_box(float3 h, float3 lo, float3 hi) {
    const float3 center = 0.5 * (hi + lo);
    const float3 extent = max(0.5 * (hi - lo), float3(1e-7, 1e-7, 1e-7));
    const float3 offset = h - center;
    const float3 units = abs(offset / extent);
    const float largest = max(units.x, max(units.y, units.z));
    return largest > 1.0 ? center + offset / largest : h;
}

/// The history at `uv`: a Catmull-Rom filter in 5 bilinear taps (the corners of the 4x4 kernel weigh little).
float4 history_at(float2 uv) {
    const float2 size = float2(params.size);
    const float2 position = uv * size;
    const float2 center = floor(position - 0.5) + 0.5;
    const float2 f = position - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 tc0 = (center - 1.0) / size;
    const float2 tc12 = (center + w2 / w12) / size;
    const float2 tc3 = (center + 2.0) / size;
    const float weights[5] = { w12.x * w0.y, w0.x * w12.y, w12.x * w12.y, w3.x * w12.y, w12.x * w3.y };
    const float2 taps[5] = { float2(tc12.x, tc0.y), float2(tc0.x, tc12.y), tc12, float2(tc3.x, tc12.y),
                             float2(tc12.x, tc3.y) };
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float total = 0.0;
    for (uint i = 0; i < 5; ++i) {
        sum += params.history.SampleLevel(params.linear_clamp, taps[i], 0.0) * weights[i];
        total += weights[i];
    }
    return sum / total;
}

/// Whether a history texel shows `surface`: the same one, wherever it moved (a moving object, the same terrain chunk),
/// or another at the depth this one would have been seen at (a neighbor, a new LOD of the same ground).
bool shows(uint2 meta, uint surface, float expected, float tolerance) {
    const uint previous = meta.x & id_mask;
    return previous == surface ||
           (surface != sky_id && previous != sky_id && abs(asfloat(meta.y) - expected) <= tolerance);
}

groupshared float4 tile_color[10][10];  ///< the group's 8x8 pixels and a border of one: YCoCg (compressed), share
groupshared float tile_depth[10][10];

[shader("compute")]
[numthreads(8, 8, 1)]
void taa_resolve(uint3 id: SV_DispatchThreadID, uint3 group: SV_GroupID, uint index: SV_GroupIndex) {
    const int2 last = int2(params.size) - 1;
    // ---- this frame's neighborhoods, read once per group.
    const int2 corner = int2(group.xy) * 8 - 1;
    for (uint i = index; i < 100; i += 64) {
        const int2 q = clamp(corner + int2(int(i % 10), int(i / 10)), int2(0, 0), last);
        const float4 c = params.radiance.Load(int3(q, 0));
        tile_color[i / 10][i % 10] = float4(compressed(c.rgb), saturate(c.a - 1.0));
        tile_depth[i / 10][i % 10] = params.depth.Load(int3(q, 0));
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id.xy >= params.size)) {
        return;
    }
    const int2 pixel = int2(id.xy);
    const int2 local = pixel - corner;

    // ---- the 3x3 neighborhood: color statistics in YCoCg, and the nearest surface (reverse-Z: largest).
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 sum_squares = float3(0.0, 0.0, 0.0);
    float3 lo = float3(1e30, 1e30, 1e30);
    float3 hi = float3(-1e30, -1e30, -1e30);
    float share_lo = 1.0;
    float share_hi = 0.0;
    float nearest = -1.0;
    int2 nearest_pixel = pixel;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const float4 t = tile_color[local.y + dy][local.x + dx];
            sum += t.xyz;
            sum_squares += t.xyz * t.xyz;
            lo = min(lo, t.xyz);
            hi = max(hi, t.xyz);
            share_lo = min(share_lo, t.w);
            share_hi = max(share_hi, t.w);
            const float d = tile_depth[local.y + dy][local.x + dx];
            if (d > nearest) {
                nearest = d;
                nearest_pixel = clamp(pixel + int2(dx, dy), int2(0, 0), last);
            }
        }
    }
    const float3 mean = sum / 9.0;
    const float3 sigma = sqrt(max(sum_squares / 9.0 - mean * mean, float3(0.0, 0.0, 0.0)));
    const float4 current = params.radiance.Load(int3(pixel, 0));

    // ---- this pixel's surface, and where it was.
    const uint2 visible = params.visibility.Load(int3(pixel, 0));
    const uint surface = visible.x == no_geometry ? sky_id : (params.instances[visible.x].id & id_mask);
    const float depth = tile_depth[local.y][local.x];
    const float linear_depth = depth > 0.0 ? params.near_plane / depth : sky_depth;
    const float2 uv = (float2(pixel) + 0.5) / float2(params.size);
    const float2 motion = params.motion.Load(int3(nearest_pixel, 0));
    const float2 previous_uv = uv + motion;
    const float moved = length(motion * float2(params.size));  // pixels

    bool valid = (params.flags & taa_history_valid) != 0 && all(previous_uv >= 0.0) && all(previous_uv <= 1.0) &&
                 !any(isnan(current.rgb)) && !any(isinf(current.rgb));
    uint samples = 0;
    bool at_center = false;  ///< the history texel it lands on shows this surface (not just one beside it)
    if (valid) {
        // Last frame's camera would have seen this surface at this depth, had the surface not moved: the matrix gives
        // last frame's clip space over this frame's w, the linear depth.
        const float4 clip = mul(params.reprojection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
        const float expected = clip.w * linear_depth;
        const float tolerance = 0.02 * expected + 0.01;
        const int2 closest = clamp(int2(round(previous_uv * float2(params.size) - 0.5)), int2(0, 0), last);
        // The texel it lands on first: most pixels find their surface there and read nothing more.
        const uint2 center_meta = params.history_meta.Load(int3(closest, 0));
        samples = center_meta.x >> 24;
        at_center = shows(center_meta, surface, expected, tolerance);
        bool kept = at_center;
        for (uint k = 0; k < 9 && !kept; ++k) {
            const int2 q = clamp(closest + int2(int(k % 3) - 1, int(k / 3) - 1), int2(0, 0), last);
            kept = k != 4 && shows(params.history_meta.Load(int3(q, 0)), surface, expected, tolerance);
        }
        valid = kept;
    }

    float3 result = current.rgb;
    float share = saturate(current.a - 1.0);
    uint count = 1;
    if (valid && samples > 0) {
        const float4 history = history_at(previous_uv);
        float ratio = params.exposure_ratio;
        if ((params.flags & taa_auto_exposure) != 0) {
            const float4 state = params.exposure_state[0];
            ratio = state.y / max(state.z, 1e-30);
        }
        // Variance clipping inside the neighborhood's min/max box: tighter than the box, so less ghosting.
        const float3 box_lo = max(lo, mean - params.clip_gamma * sigma);
        const float3 box_hi = min(hi, mean + params.clip_gamma * sigma);
        const float3 h = clip_to_box(compressed(history.rgb * ratio), box_lo, box_hi);
        const float history_share = clamp(saturate(history.a - 1.0), share_lo, share_hi);
        count = min(samples + 1, max_samples);
        const float still_or_moving =
            lerp(params.min_weight, params.moving_weight, saturate(moved / params.moving_pixels));
        const float edge = at_center ? 0.0 : params.edge_weight * saturate(moved - 0.5);
        const float w = max(max(1.0 / float(count), still_or_moving), edge);
        result = expand(max(from_ycocg(lerp(h, compressed(current.rgb), w)), float3(0.0, 0.0, 0.0)));
        share = lerp(history_share, share, w);
    }
    result = min(result, float3(fp16_max, fp16_max, fp16_max));  // NaN stays: the exposure pass counts it
    const float4 out_color = float4(result, 1.0 + share);
    params.resolved[pixel] = out_color;
    // A NaN or infinite pixel never enters the history (it would stay forever): it starts over next frame.
    const bool finite = !any(isnan(result)) && !any(isinf(result));
    params.history_out[pixel] = finite ? out_color : float4(0.0, 0.0, 0.0, 1.0);
    params.meta_out[pixel] = uint2(surface | ((finite ? count : 0u) << 24), asuint(linear_depth));
}

/// After the resolve, on the displayed image only (the history stays unsharpened): every reprojection resamples the
/// history between texels, which softens it a little each frame while the camera moves (never while it is still).
/// A limited unsharp mask on the 4 neighbors, in the compressed space, kept within their range so it cannot ring,
/// scaled by how far the pixel moves (`radiance` is the resolve's output, `resolved` this pass's).
[shader("compute")]
[numthreads(8, 8, 1)]
void taa_sharpen(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    const int2 pixel = int2(id.xy);
    const int2 last = int2(params.size) - 1;
    const float4 c = params.radiance.Load(int3(pixel, 0));
    const float2 motion = params.motion.Load(int3(pixel, 0)) * float2(params.size);
    const float k = params.sharpness * saturate(length(motion));
    if (k <= 0.0 || any(isnan(c.rgb)) || any(isinf(c.rgb))) {
        params.resolved[pixel] = c;
        return;
    }
    const float3 center = compress(max(c.rgb, float3(0.0, 0.0, 0.0)));
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 lo = center;
    float3 hi = center;
    const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    for (uint i = 0; i < 4; ++i) {
        const int2 q = clamp(pixel + offsets[i], int2(0, 0), last);
        const float3 n = compress(max(params.radiance.Load(int3(q, 0)).rgb, float3(0.0, 0.0, 0.0)));
        sum += n;
        lo = min(lo, n);
        hi = max(hi, n);
    }
    const float3 sharpened = clamp(center + k * (center - 0.25 * sum), lo, hi);
    params.resolved[pixel] = float4(min(expand(sharpened), float3(fp16_max, fp16_max, fp16_max)), c.a);
}
