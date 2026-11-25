// The atmosphere's lookup tables (ADR 0050), after Hillaire 2020. One compute entry per table; all read this frame's
// SkyFrame (veach/atmosphere.slang). Mirrors render::AtmosphereLutParams.

import veach.atmosphere;
import veach.bindless;

struct AtmosphereLutParams {
    DescriptorHandle<StructuredBuffer<SkyFrame>> sky;
    DescriptorHandle<RWTexture2D<float4>> output;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<AtmosphereLutParams> lut_params;

float3 sun_transmittance(SkyFrame s, float3 p) {
    const float r = length(p);
    const float mu = dot(p / r, s.sun.xyz);
    if (ray_sphere(p, s.sun.xyz, s.atmosphere.planet.x) > 0.0) {
        return float3(0.0, 0.0, 0.0);  // the planet's shadow
    }
    return sample_transmittance(s, r, mu);
}

float3 multiple_scattering(SkyFrame s, float3 p) {
    const float r = length(p);
    const float mu = dot(p / r, s.sun.xyz);
    const float2 uv = float2(mu * 0.5 + 0.5, saturate((r - s.atmosphere.planet.x) / (s.atmosphere.planet.y - s.atmosphere.planet.x)));
    return s.multiscattering.SampleLevel(s.linear_clamp, uv, 0.0).rgb;
}

// ---- transmittance -----------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void transmittance_lut(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= lut_params.size)) {
        return;
    }
    const SkyFrame s = lut_params.sky[0];
    float r;
    float mu;
    transmittance_r_mu(s.atmosphere, (float2(id.xy) + 0.5) / float2(lut_params.size), r, mu);
    lut_params.output[id.xy] = float4(integrate_transmittance(s.atmosphere, r, mu), 1.0);
}

// ---- multiple scattering (Hillaire 2020, section 5.5) -------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void multiscattering_lut(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= lut_params.size)) {
        return;
    }
    const SkyFrame s0 = lut_params.sky[0];
    const float2 uv = (float2(id.xy) + 0.5) / float2(lut_params.size);
    const float sun_cos = uv.x * 2.0 - 1.0;
    const float height = max(uv.y * (s0.atmosphere.planet.y - s0.atmosphere.planet.x), 0.01);
    SkyFrame s = s0;
    s.sun.xyz = float3(sqrt(max(1.0 - sun_cos * sun_cos, 0.0)), sun_cos, 0.0);  // in the LUT's own frame
    const float3 origin = float3(0.0, s.atmosphere.planet.x + height, 0.0);
    const float isotropic = 1.0 / (4.0 * atmosphere_pi);
    float3 second_order = float3(0.0, 0.0, 0.0);
    float3 transfer = float3(0.0, 0.0, 0.0);
    const uint directions = 8;
    for (uint i = 0; i < directions; ++i) {
        for (uint j = 0; j < directions; ++j) {
            const float cos_theta = 1.0 - 2.0 * (float(i) + 0.5) / float(directions);
            const float phi = 2.0 * atmosphere_pi * (float(j) + 0.5) / float(directions);
            const float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
            const float3 direction = float3(sin_theta * cos(phi), cos_theta, sin_theta * sin(phi));
            const float ground = ray_sphere(origin, direction, s.atmosphere.planet.x);
            const float top = ray_sphere(origin, direction, s.atmosphere.planet.y);
            const float length_km = ground > 0.0 ? ground : top;
            const uint steps = 20;
            const float dt = max(length_km, 0.0) / float(steps);
            float3 throughput = float3(1.0, 1.0, 1.0);
            float3 l = float3(0.0, 0.0, 0.0);
            float3 lf = float3(0.0, 0.0, 0.0);
            for (uint k = 0; k < steps; ++k) {
                const float3 p = origin + direction * ((float(k) + 0.5) * dt);
                const Medium m = medium_at(s.atmosphere, length(p) - s.atmosphere.planet.x);
                const float3 step_transmittance = exp(-m.extinction * dt);
                const float3 in_scatter = m.scattering * isotropic * sun_transmittance(s, p);
                const float3 extinction = max(m.extinction, 1e-9);
                l += throughput * (in_scatter - in_scatter * step_transmittance) / extinction;
                lf += throughput * (m.scattering - m.scattering * step_transmittance) / extinction;
                throughput *= step_transmittance;
            }
            if (ground > 0.0) {  // light the ground reflects (Lambertian, the ground albedo)
                const float3 p = origin + direction * ground;
                const float3 n = normalize(p);
                l += throughput * sun_transmittance(s, p + n * 0.001) * saturate(dot(n, s.sun.xyz)) *
                     s.atmosphere.planet.z / atmosphere_pi;
            }
            second_order += l / float(directions * directions);
            transfer += lf * isotropic * 4.0 * atmosphere_pi / float(directions * directions);
        }
    }
    lut_params.output[id.xy] = float4(second_order / max(1.0 - transfer, 1e-3), 1.0);
}

// ---- the sky around the camera -------------------------------------------------------------------------------------

/// In-scattered luminance (per lux) and transmittance along a ray from `origin`, up to `max_km`.
void integrate_scattering(SkyFrame s, float3 origin, float3 direction, float max_km, uint steps, out float3 luminance,
                          out float3 transmittance) {
    const float ground = ray_sphere(origin, direction, s.atmosphere.planet.x);
    const float top = ray_sphere(origin, direction, s.atmosphere.planet.y);
    float length_km = ground > 0.0 ? ground : top;
    length_km = min(max(length_km, 0.0), max_km);
    const float dt = length_km / float(steps);
    const float cos_theta = dot(direction, s.sun.xyz);
    const float phase_r = rayleigh_phase(cos_theta);
    const float phase_m = mie_phase(cos_theta, s.atmosphere.mie.w);
    luminance = float3(0.0, 0.0, 0.0);
    transmittance = float3(1.0, 1.0, 1.0);
    for (uint k = 0; k < steps; ++k) {
        const float3 p = origin + direction * ((float(k) + 0.5) * dt);
        const Medium m = medium_at(s.atmosphere, length(p) - s.atmosphere.planet.x);
        const float3 step_transmittance = exp(-m.extinction * dt);
        const float3 single = (m.rayleigh * phase_r + m.mie * phase_m) * sun_transmittance(s, p);
        const float3 in_scatter = single + m.scattering * multiple_scattering(s, p);
        luminance += transmittance * (in_scatter - in_scatter * step_transmittance) / max(m.extinction, 1e-9);
        transmittance *= step_transmittance;
    }
}

float3 camera_origin(SkyFrame s) {
    return float3(0.0, s.atmosphere.planet.x + max(s.camera.x, 0.001), 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void sky_view_lut(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= lut_params.size)) {
        return;
    }
    const SkyFrame s = lut_params.sky[0];
    const float3 direction = sky_view_direction(s, (float2(id.xy) + 0.5) / float2(lut_params.size));
    float3 luminance;
    float3 transmittance;
    integrate_scattering(s, camera_origin(s), direction, 1e9, 30, luminance, transmittance);
    lut_params.output[id.xy] = float4(luminance, 1.0);
}

/// 32 depth slices of 32 x 32 screen cells, side by side (1024 x 32): in-scattering per lux and mean transmittance.
[shader("compute")]
[numthreads(8, 8, 1)]
void aerial_lut(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= lut_params.size)) {
        return;
    }
    const SkyFrame s = lut_params.sky[0];
    const uint slice = id.x / 32;
    const float2 uv = (float2(id.x % 32, id.y) + 0.5) / 32.0;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 near = mul(s.inverse_view_projection, float4(ndc, 1.0, 1.0));  // reverse-Z
    const float4 mid = mul(s.inverse_view_projection, float4(ndc, 0.5, 1.0));
    const float3 direction = normalize(mid.xyz / mid.w - near.xyz / near.w);
    const float t = (float(slice) + 0.5) / float(aerial_slices);
    const float depth_km = aerial_depth_km * t * t;
    float3 luminance;
    float3 transmittance;
    integrate_scattering(s, camera_origin(s), direction, depth_km, 8, luminance, transmittance);
    lut_params.output[id.xy] = float4(luminance, dot(transmittance, float3(1.0, 1.0, 1.0)) / 3.0);
}

/// The sky's cosine-weighted average over the upper hemisphere, for an upward normal (the ambient stopgap).
[shader("compute")]
[numthreads(1, 1, 1)]
void sky_irradiance(uint3 id: SV_DispatchThreadID) {
    const SkyFrame s = lut_params.sky[0];
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    const uint n = 16;
    for (uint i = 0; i < n; ++i) {
        for (uint j = 0; j < n; ++j) {
            // Cosine-distributed directions: the plain average is the cosine-weighted one.
            const float u = (float(i) + 0.5) / float(n);
            const float phi = 2.0 * atmosphere_pi * (float(j) + 0.5) / float(n);
            const float r = sqrt(u);
            const float3 direction = float3(r * cos(phi), sqrt(1.0 - u), r * sin(phi));
            sum += s.sky_view.SampleLevel(s.linear_clamp, sky_view_uv(s, direction), 0.0).rgb;
            weight += 1.0;
        }
    }
    lut_params.output[uint2(0, 0)] = float4(sum / weight, 1.0);
}
