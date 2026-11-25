// Veach's temporal upscaler (ADR 0043): jittered render-resolution frames into an anti-aliased output-resolution
// image, on every backend (MetalFX's scaler replaces it on Apple GPUs that support it). One pass per output pixel:
//   - the current frame, reconstructed from the 3x3 nearest render samples with a Gaussian on their jittered
//     positions;
//   - history, sampled with a 5-tap Catmull-Rom at the reprojected position (motion of the nearest surface in the
//     3x3, so edges carry their foreground's motion), dropped where the surface pass found a disocclusion
//     (veach/temporal.slang), clipped to the current neighborhood's YCoCg variance box;
//   - blended with weight max(0.04, 1 / (samples + 1)), luminance-weighted against fireflies.
// Input and output are exposed linear color; alpha carries the number of frames in the history (capped).

import veach.bindless;
import veach.temporal;

struct Params {
    DescriptorHandle<Texture2D<float4>> color;     ///< render resolution, exposed linear
    DescriptorHandle<Texture2D<float4>> motion;    ///< render resolution: xy previous minus current, w valid taps
    DescriptorHandle<Texture2D<float4>> position;  ///< render resolution: w = linear depth (0: no geometry)
    DescriptorHandle<Texture2D<float4>> history;   ///< output resolution, last frame's output
    DescriptorHandle<RWTexture2D<float4>> output;  ///< output resolution: this frame's, next frame's history
    DescriptorHandle<SamplerState> linear_clamp;
    uint2 render_size;
    uint2 output_size;
    float2 jitter;  ///< this frame's sample offset from the render pixel center, render pixels
    uint reset;     ///< 1: no history this frame
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

static const float max_history = 25.0;

float3 to_ycocg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
float3 from_ycocg(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float4 tap(float2 uv) {
    return params.history.SampleLevel(params.linear_clamp, uv, 0.0);
}

/// History at `uv` with a 5-tap Catmull-Rom (Jimenez's bilinear trick): sharp without ringing much.
float4 sample_history(float2 uv) {
    const float2 size = float2(params.output_size);
    const float2 position = uv * size;
    const float2 center = floor(position - 0.5) + 0.5;
    const float2 f = position - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 tc0 = (center - 1.0) / size;
    const float2 tc3 = (center + 2.0) / size;
    const float2 tc12 = (center + w2 / w12) / size;
    float4 result = tap(float2(tc12.x, tc0.y)) * (w12.x * w0.y) + tap(float2(tc0.x, tc12.y)) * (w0.x * w12.y) +
                    tap(float2(tc12.x, tc12.y)) * (w12.x * w12.y) + tap(float2(tc3.x, tc12.y)) * (w3.x * w12.y) +
                    tap(float2(tc12.x, tc3.y)) * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(result / weight, 0.0);
}

float luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void taau(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.output_size)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(params.output_size);
    const float2 r = uv * float2(params.render_size);  // this output pixel's center, in render pixels
    const int2 nearest = int2(floor(r - params.jitter));

    // The current frame around this pixel: Gaussian reconstruction, neighborhood moments, nearest surface.
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight_sum = 0.0;
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);
    float nearest_depth = 1e30;
    int2 nearest_pixel = clamp(nearest, int2(0, 0), int2(params.render_size) - 1);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(nearest + int2(x, y), int2(0, 0), int2(params.render_size) - 1);
            const float3 c = params.color.Load(int3(q, 0)).rgb;
            const float2 d = float2(q) + 0.5 + params.jitter - r;
            const float w = exp(-2.29 * dot(d, d));
            const float tone = 1.0 / (1.0 + luma(c));  // luminance weighting keeps fireflies from spreading
            sum += c * w * tone;
            weight_sum += w * tone;
            const float3 ycocg = to_ycocg(c);
            m1 += ycocg;
            m2 += ycocg * ycocg;
            const float depth = params.position.Load(int3(q, 0)).w;
            if (depth > 0.0 && depth < nearest_depth) {
                nearest_depth = depth;
                nearest_pixel = q;
            }
        }
    }
    const float3 current = sum / max(weight_sum, 1e-6);
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    // History at the reprojected position, if the surface was there last frame.
    const float4 motion = params.motion.Load(int3(nearest_pixel, 0));
    const History h = history_of(uint2(nearest_pixel), motion);
    const float2 previous_uv = uv + motion.xy / float2(params.render_size);
    const bool on_screen = all(previous_uv >= 0.0) && all(previous_uv <= 1.0);
    float samples = 0.0;
    float3 result = current;
    if (params.reset == 0 && h.valid && on_screen) {
        const float4 history = sample_history(previous_uv);
        samples = min(params.history.Load(int3(int2(previous_uv * float2(params.output_size)), 0)).a, max_history);
        // Clip toward the neighborhood mean: whatever history holds outside the box is from another surface.
        const float3 box_min = mean - sigma;
        const float3 box_max = mean + sigma;
        const float3 y = to_ycocg(history.rgb);
        const float3 to_clip = y - mean;
        const float3 extent = max(box_max - mean, 1e-5);
        const float3 unit = abs(to_clip / extent);
        const float ratio = max(unit.x, max(unit.y, unit.z));
        const float3 clipped = from_ycocg(ratio > 1.0 ? mean + to_clip / ratio : y);
        const float alpha = max(0.04, 1.0 / (samples + 1.0));
        const float w_current = alpha / (1.0 + luma(current));
        const float w_history = (1.0 - alpha) / (1.0 + luma(clipped));
        result = (current * w_current + clipped * w_history) / (w_current + w_history);
    }
    params.output[id.xy] = float4(result, min(samples + 1.0, max_history));
}
