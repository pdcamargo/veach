// The space backdrop on the real-time image (ADR 0091, docs/specs/space-backdrop/spec.md): after the shade pass and
// temporal AA, on the sky pixels the shade pass marked (alpha 1 + the share of the pixel that shows the sky behind
// which the backdrop is: 2 where it all does, in between at edges temporal AA smoothed), the baked texture and the
// stars (veach.backdrop), through the air (sky_to_space), at the creator's gain, times that share, added to the exposed
// radiance. Drawn after temporal AA along the unjittered rays, so stars stay crisp points that neither flicker nor
// smear (ADR 0061, docs/specs/temporal-aa/spec.md). Alpha becomes the share of the pixel that auto-exposure meters:
// all but the backdrop. Its parameter layout matches render::BackdropCompositeParams (realtime_renderer.cpp).

import veach.atmosphere;
import veach.backdrop;
import veach.bindless;

struct CompositeParams {
    float4x4 inverse_view_projection;  ///< without jitter
    DescriptorHandle<RWTexture2D<float4>> radiance;  ///< exposed, alpha 1 + the share of the pixel to draw into
    DescriptorHandle<StructuredBuffer<SkyFrame>> sky;
    DescriptorHandle<StructuredBuffer<float4>> exposure_state;  ///< with composite_auto_exposure: state[0].y
    uint2 size;
    float exposure;  ///< the camera's, without auto-exposure
    uint flags;      ///< composite_auto_exposure
};
[[vk::push_constant]] ConstantBuffer<CompositeParams> composite;

static const uint composite_auto_exposure = 1u;

[shader("compute")]
[numthreads(8, 8, 1)]
void backdrop_composite(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= composite.size)) {
        return;
    }
    const float4 c = composite.radiance[id.xy];
    const float coverage = saturate(c.a - 1.0);
    if (coverage <= 0.0) {
        return;  // geometry, or no backdrop behind the sky
    }
    const SkyFrame sky = composite.sky[0];
    const float exposure =
        (composite.flags & composite_auto_exposure) != 0 ? composite.exposure_state[0].y : composite.exposure;
    // The pixel's ray, as the shade pass's camera_ray.
    const float2 pixel = float2(id.xy) + 0.5;
    const float2 ndc = float2(pixel.x / float(composite.size.x) * 2.0 - 1.0, 1.0 - pixel.y / float(composite.size.y) * 2.0);
    const float4 near = mul(composite.inverse_view_projection, float4(ndc, 1.0, 1.0));
    const float4 mid = mul(composite.inverse_view_projection, float4(ndc, 0.5, 1.0));
    const float3 direction = normalize(mid.xyz / mid.w - near.xyz / near.w);

    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    const float sky_luminance = dot(c.rgb, luma);
    float3 beyond = float3(0.0, 0.0, 0.0);
    const float3 to_space = sky_to_space(sky, direction);
    if (any(to_space > 0.0)) {
        beyond = backdrop_radiance(sky, direction) * to_space *
                 (backdrop_gain(sky, exposure, sky_luminance) * exposure * coverage);
    }
    const float total = sky_luminance + dot(beyond, luma);
    composite.radiance[id.xy] =
        float4(min(c.rgb + beyond, float3(65504.0, 65504.0, 65504.0)), total > 0.0 ? sky_luminance / total : 1.0);
}
