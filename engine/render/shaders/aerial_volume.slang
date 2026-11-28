// The aerial-perspective volume (ADR 0076 decision 3, ADR 0091): one thread per screen cell marches its view ray through
// the air once and writes every depth slice, in-scattered luminance (per lux of sun.w) in rgb and the mean
// transmittance in a. Slices follow the ray's own span through the atmosphere (aerial_ray, aerial_x in
// veach/atmosphere.slang), so the volume covers whatever the body's air is from where the camera is. Every sun with
// its eclipses, and the sky visibility of the camera's surroundings (air within their reach scatters only the sky's
// open fraction: no haze in caves). Mirrors render::AerialVolumeParams (atmosphere.cpp).

import veach.atmosphere;
import veach.bindless;

struct AerialVolumeParams {
    DescriptorHandle<StructuredBuffer<SkyFrame>> sky;
    DescriptorHandle<RWTexture3D<float4>> output;
    uint4 size;  ///< xyz = cells across, down, slices
};
[[vk::push_constant]] ConstantBuffer<AerialVolumeParams> volume_params;

/// Sub-steps per slice: the medium's exponential is integrated analytically between them.
static const uint aerial_substeps = 1;

[shader("compute")]
[numthreads(8, 8, 1)]
void aerial_volume(uint3 id: SV_DispatchThreadID) {
    const uint3 size = volume_params.size.xyz;
    if (any(id.xy >= size.xy)) {
        return;
    }
    const SkyFrame s = volume_params.sky[0];
    const float3 direction = aerial_cell_direction(s, (float2(id.xy) + 0.5) / float2(size.xy));
    const AerialRay ray = aerial_ray(s, direction);
    if (!ray.valid) {
        for (uint k = 0; k < size.z; ++k) {
            volume_params.output[uint3(id.xy, k)] = float4(0.0, 0.0, 0.0, 1.0);
        }
        return;
    }
    const float bottom = s.atmosphere.planet.x;
    const float top_height = s.atmosphere.planet.y - bottom;
    const SunPhases ph = sun_phases(s, direction);
    float3 luminance = float3(0.0, 0.0, 0.0);
    float3 transmittance = float3(1.0, 1.0, 1.0);
    float previous = 0.0;  // km into the air
    for (uint k = 0; k < size.z; ++k) {
        // The last slice holds the whole path through the shell.
        const float end = k + 1 == size.z ? ray.full : aerial_x((float(k) + 0.5) / float(size.z)) * ray.span;
        const float dt = (end - previous) / float(aerial_substeps);
        for (uint j = 0; j < aerial_substeps; ++j) {
            const float t = ray.t_in + previous + (float(j) + 0.5) * dt;  // km from the camera
            const float3 p = s.origin.xyz + direction * t;
            const float height = length(p) - bottom;
            if (height > top_height || height < -top_height) {
                continue;  // past the top, or deep under the ground where nothing is seen: no air
            }
            const Medium m = medium_at(s.atmosphere, height);
            const float3 step_transmittance = exp(-m.extinction * dt);
            const float3 in_scatter = planet_in_scatter(s, p, m, ph) * sky_visibility_at(s, t);
            luminance += transmittance * (in_scatter - in_scatter * step_transmittance) / max(m.extinction, 1e-9);
            transmittance *= step_transmittance;
        }
        previous = end;
        volume_params.output[uint3(id.xy, k)] = float4(luminance, dot(transmittance, float3(1.0, 1.0, 1.0)) / 3.0);
    }
}
