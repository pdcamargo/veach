// Bloom (docs/specs/bloom/spec.md): the glare of a lens or an eye, on pre-exposed radiance after temporal AA and the
// space backdrop, before the display transform. After Jimenez's Call of Duty bloom (SIGGRAPH 2014):
//
// - `bloom_downsample` halves the image into level 0, then each level into the next, with a 13-tap filter (five
//   overlapping 2x2 boxes read with 13 bilinear taps); into level 0 each box is weighted by Karis's 1 / (1 + luma), so a
//   lone bright pixel (a glint, a firefly) cannot make the glow flicker;
// - `bloom_upsample` goes back up: each level becomes lerp(tent(the coarser result), this level, blend), a 3x3 tent
//   one coarser texel wide, with blend = w_k / (w_k + w_k+1 + ...), so level 0 ends as the normalized weighted
//   average of every level's blur (render::bloom_chain picks the weights from each level's size on the screen);
// - the display pass (display.slang) reads level 0 with another tent and shows lerp(image, bloom, intensity).
//
// Threshold-free and energy-conserving: a uniform region is unchanged, and the glow takes from each pixel what it
// spreads around it. Values stay finite and within the fp16 maximum: non-finite input reads as 0 (auto-exposure still
// counts it), and every result is an average. Layout matches render::BloomParams (bloom.cpp).

import veach.bindless;

static const uint bloom_karis = 1u;  ///< flags: the first downsample, from the image

struct BloomParams {
    DescriptorHandle<Texture2D<float4>> source;  ///< the image or the finer level (down); this level (up)
    DescriptorHandle<Texture2D<float4>> coarse;  ///< up: the coarser level's result
    DescriptorHandle<RWTexture2D<float4>> target;
    DescriptorHandle<SamplerState> linear_clamp;
    float2 source_texel;  ///< 1 / the source's size
    float2 coarse_texel;  ///< up: 1 / the coarser level's size
    uint2 extent;         ///< the target's size
    float blend;          ///< up: this level's share of it and everything coarser
    uint flags;
};
[[vk::push_constant]] ConstantBuffer<BloomParams> bloom;

static const float3 luma = float3(0.2126, 0.7152, 0.0722);

float3 fetch(float2 uv) {
    const float3 c = bloom.source.SampleLevel(bloom.linear_clamp, uv, 0.0).rgb;
    return all(isfinite(c)) ? c : float3(0.0, 0.0, 0.0);
}

float karis(float3 c) {
    return 1.0 / (1.0 + dot(c, luma));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_downsample(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= bloom.extent)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(bloom.extent);
    const float2 t = bloom.source_texel;
    // a . b . c
    // . j . k .
    // d . e . f
    // . l . m .
    // g . h . i
    const float3 a = fetch(uv + t * float2(-2.0, -2.0));
    const float3 b = fetch(uv + t * float2(0.0, -2.0));
    const float3 c = fetch(uv + t * float2(2.0, -2.0));
    const float3 d = fetch(uv + t * float2(-2.0, 0.0));
    const float3 e = fetch(uv);
    const float3 f = fetch(uv + t * float2(2.0, 0.0));
    const float3 g = fetch(uv + t * float2(-2.0, 2.0));
    const float3 h = fetch(uv + t * float2(0.0, 2.0));
    const float3 i = fetch(uv + t * float2(2.0, 2.0));
    const float3 j = fetch(uv + t * float2(-1.0, -1.0));
    const float3 k = fetch(uv + t * float2(1.0, -1.0));
    const float3 l = fetch(uv + t * float2(-1.0, 1.0));
    const float3 m = fetch(uv + t * float2(1.0, 1.0));
    // The five boxes: the center one weighs 0.5, the four corner ones 0.125 each.
    const float3 center = (j + k + l + m) * 0.25;
    const float3 box0 = (a + b + d + e) * 0.25;
    const float3 box1 = (b + c + e + f) * 0.25;
    const float3 box2 = (d + e + g + h) * 0.25;
    const float3 box3 = (e + f + h + i) * 0.25;
    float3 result;
    if ((bloom.flags & bloom_karis) != 0) {
        const float wc = 0.5 * karis(center);
        const float w0 = 0.125 * karis(box0);
        const float w1 = 0.125 * karis(box1);
        const float w2 = 0.125 * karis(box2);
        const float w3 = 0.125 * karis(box3);
        result = (center * wc + box0 * w0 + box1 * w1 + box2 * w2 + box3 * w3) / (wc + w0 + w1 + w2 + w3);
    } else {
        result = center * 0.5 + (box0 + box1 + box2 + box3) * 0.125;
    }
    bloom.target[id.xy] = float4(result, 1.0);
}

float3 coarse_at(float2 uv) {
    return bloom.coarse.SampleLevel(bloom.linear_clamp, uv, 0.0).rgb;
}

/// The 3x3 tent (1 2 1 / 2 4 2 / 1 2 1) / 16 of the coarser level, one of its texels wide, at `uv`.
float3 tent(float2 uv) {
    const float2 t = bloom.coarse_texel;
    float3 sum = coarse_at(uv) * 4.0;
    sum += (coarse_at(uv + float2(-t.x, 0.0)) + coarse_at(uv + float2(t.x, 0.0)) + coarse_at(uv + float2(0.0, -t.y)) +
            coarse_at(uv + float2(0.0, t.y))) *
           2.0;
    sum += coarse_at(uv - t) + coarse_at(uv + t) + coarse_at(uv + float2(-t.x, t.y)) + coarse_at(uv + float2(t.x, -t.y));
    return sum / 16.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_upsample(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= bloom.extent)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(bloom.extent);
    const float3 coarser = tent(uv);
    const float3 here = bloom.source.Load(int3(id.xy, 0)).rgb;
    bloom.target[id.xy] = float4(lerp(coarser, here, bloom.blend), 1.0);
}
