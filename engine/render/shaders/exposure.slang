// Spike SP-14 (VCH-090), throwaway (docs/research/spikes/sp-14-exposure-depth.md): histogram auto-exposure that stays
// on the GPU, with pre-exposure (ADR 0063). The shade pass writes radiance times last frame's exposure (the
// pre-exposure, state[0].y); `exposure_histogram` bins each pixel's absolute luminance by EV100 (log2(L * 100 / 12.5))
// into 128 bins and counts non-finite and fp16-saturated pixels; `exposure_resolve` meters the percentile band (the
// mean EV of the pixels between low_percent and high_percent), raises it if needed so the highlight percentile is not
// clipped, adapts in EV and writes the new exposure, which the display pass applies as state[0].y / state[0].z. Nothing is read
// back to drive it. Its parameter layout matches render::ExposureParams (realtime_renderer.cpp).

import veach.bindless;

static const uint exposure_bins = 128;
static const uint counter_nonfinite = 128;  ///< pixels with a NaN or infinite channel
static const uint counter_saturated = 129;  ///< pixels with a channel at the fp16 maximum (clamped)
static const uint counter_below = 130;      ///< pixels darker than the histogram (black space): not metered
static const uint histogram_size = 132;
static const float fp16_max = 65504.0;
// AgX's input range (display.slang): exposed luminance above this is white, below the floor black.
static const float agx_white = 16.2917;     // 2^4.026069
static const float agx_black = 0.000175;    // 2^-12.47393

struct ExposureParams {
    DescriptorHandle<Texture2D<float4>> radiance;           ///< pre-exposed (the shade pass's output)
    DescriptorHandle<RWStructuredBuffer<uint>> histogram;   ///< 128 bins, then the counters above
    DescriptorHandle<RWStructuredBuffer<float4>> state;     ///< 4 elements, see exposure_resolve
    uint2 size;
    float histogram_min_ev;
    float histogram_max_ev;
    float min_ev100;
    float max_ev100;
    float low_percent;
    float high_percent;
    float speed_to_bright;
    float speed_to_dark;
    float max_rate_to_bright;
    float max_rate_to_dark;
    float compensation;
    float delta_time;
    float highlight_percent;  ///< this percentile of the metered pixels is kept at or below `highlight_white`
    float highlight_white;    ///< exposed luminance (AgX shows 16.3 as white)
    uint reset;
    float highlight_max_pull;  ///< the most the highlight may darken the metered exposure (stops)
};
[[vk::push_constant]] ConstantBuffer<ExposureParams> params;

groupshared uint local_bins[histogram_size];

[shader("compute")]
[numthreads(16, 16, 1)]
void exposure_histogram(uint3 id: SV_DispatchThreadID, uint index: SV_GroupIndex) {
    if (index < histogram_size) {
        local_bins[index] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < params.size)) {
        const float4 pixel = params.radiance.Load(int3(id.xy, 0));
        const float3 c = pixel.rgb;
        if (any(isnan(c)) || any(isinf(c))) {
            InterlockedAdd(local_bins[counter_nonfinite], 1u);
        } else {
            if (any(c >= fp16_max)) {
                InterlockedAdd(local_bins[counter_saturated], 1u);
            }
            const float pre_exposure = params.state[0].y;
            // Alpha is the metered share: the space backdrop, exposed on its own (ADR 0091), is left out.
            const float luminance = dot(c, float3(0.2126, 0.7152, 0.0722)) * saturate(pixel.a) / pre_exposure;
            const float ev = log2(max(luminance, 1e-30) * 8.0);
            if (ev < params.histogram_min_ev) {
                InterlockedAdd(local_bins[counter_below], 1u);
            } else {
                const float t = (ev - params.histogram_min_ev) / (params.histogram_max_ev - params.histogram_min_ev);
                const uint bin = min(uint(t * float(exposure_bins)), exposure_bins - 1);
                InterlockedAdd(local_bins[bin], 1u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < histogram_size && local_bins[index] != 0) {
        InterlockedAdd(params.histogram[index], local_bins[index]);
    }
}

groupshared uint resolve_bins[histogram_size];

float bin_ev(uint bin) {
    return params.histogram_min_ev +
           (float(bin) + 0.5) / float(exposure_bins) * (params.histogram_max_ev - params.histogram_min_ev);
}

/// state[0] = (ev100, exposure, the pre-exposure this frame was shaded with, 1 once initialized)
/// state[1] = (target ev100, metered ev100 before the range clamp, the band's low and high ev)
/// state[2] = (non-finite, saturated, below-histogram, metered pixels)
/// state[3] = (fraction of pixels AgX shows white, black at the new exposure; metered fraction; highlight EV100)
[shader("compute")]
[numthreads(128, 1, 1)]
void exposure_resolve(uint index: SV_GroupIndex) {
    resolve_bins[index] = params.histogram[index];
    if (index < histogram_size - exposure_bins) {
        resolve_bins[exposure_bins + index] = params.histogram[exposure_bins + index];
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        float total = 0.0;
        for (uint b = 0; b < exposure_bins; ++b) {
            total += float(resolve_bins[b]);
        }
        const float pixels = float(params.size.x) * float(params.size.y);
        const float4 previous = params.state[0];
        float metered = params.min_ev100;
        float band_low = params.min_ev100;
        float band_high = params.min_ev100;
        float highlight = params.min_ev100;  ///< the lowest EV100 that keeps the highlight percentile unclipped
        if (total >= max(1.0, 0.0005 * pixels)) {  // less than that is darkness: the lowest exposure value
            const float lo = params.low_percent * total;
            const float hi = params.high_percent * total;
            const float top = params.highlight_percent * total;
            const float bin_width = (params.histogram_max_ev - params.histogram_min_ev) / float(exposure_bins);
            bool highlight_found = false;
            float below = 0.0;
            float sum = 0.0;
            float weight = 0.0;
            bool low_found = false;
            for (uint b = 0; b < exposure_bins; ++b) {
                const float count = float(resolve_bins[b]);
                const float inside = max(0.0, min(below + count, hi) - max(below, lo));
                if (inside > 0.0) {
                    sum += inside * bin_ev(b);
                    weight += inside;
                    if (!low_found) {
                        band_low = bin_ev(b);
                        low_found = true;
                    }
                    band_high = bin_ev(b);
                }
                if (!highlight_found && below + count >= top) {
                    // Its upper edge exposed to highlight_white: 2^ev / 8 / (1.2 * 2^EV) = white.
                    highlight = bin_ev(b) + 0.5 * bin_width - log2(params.highlight_white * 1.2 * 8.0);
                    highlight_found = true;
                }
                below += count;
            }
            metered = weight > 0.0 ? sum / weight : params.min_ev100;
        }
        const float average = metered + params.compensation;
        const float target = clamp(max(average, min(highlight, average + params.highlight_max_pull)), params.min_ev100,
                                   params.max_ev100);
        float ev = previous.x;
        if (params.reset != 0 || previous.w == 0.0) {
            ev = target;
        } else {
            const float delta = target - ev;
            const bool brighter = delta > 0.0;
            const float speed = brighter ? params.speed_to_bright : params.speed_to_dark;
            const float rate = (brighter ? params.max_rate_to_bright : params.max_rate_to_dark) * params.delta_time;
            ev += clamp(delta * (1.0 - exp(-params.delta_time * speed)), -rate, rate);
        }
        const float exposure = 1.0 / (1.2 * exp2(ev));
        float white = 0.0;
        float black = 0.0;
        for (uint b = 0; b < exposure_bins; ++b) {
            const float exposed = exp2(bin_ev(b)) / 8.0 * exposure;
            white += exposed > agx_white ? float(resolve_bins[b]) : 0.0;
            black += exposed < agx_black ? float(resolve_bins[b]) : 0.0;
        }
        params.state[0] = float4(ev, exposure, previous.y, 1.0);
        params.state[1] = float4(target, metered, band_low, band_high);
        params.state[2] = float4(float(resolve_bins[counter_nonfinite]), float(resolve_bins[counter_saturated]),
                                 float(resolve_bins[counter_below]), total);
        params.state[3] = float4(white / pixels, black / pixels, total / pixels, highlight);
    }
    // Clear for the next frame (every thread has read its bins above).
    params.histogram[index] = 0;
    if (index < histogram_size - exposure_bins) {
        params.histogram[exposure_bins + index] = 0;
    }
}
