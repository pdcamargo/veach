// The atmosphere's lookup tables (ADR 0038, 0063), after Hillaire 2020. The aerial-perspective volume is
// aerial_volume.slang. One compute entry per table (the ambient table: veach/ambient.slang); all read this frame's
// SkyFrame (veach/atmosphere.slang). Mirrors render::AtmosphereLutParams.

import veach.ambient;
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
    if (sphere_blocks(p, s.sun.xyz, s.atmosphere.planet.x)) {
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
            const float r = s.atmosphere.planet.x + height;
            const float ground = ray_sphere_at(origin, direction, r, s.atmosphere.planet.x);
            const float top = ray_sphere_at(origin, direction, r, s.atmosphere.planet.y);
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
    const float r = s.atmosphere.planet.x + max(s.camera.x, 0.001);  // origin is camera_origin(s)
    const float ground = ray_sphere_at(origin, direction, r, s.atmosphere.planet.x);
    const float top = ray_sphere_at(origin, direction, r, s.atmosphere.planet.y);
    float length_km = ground > 0.0 ? ground : top;
    length_km = min(max(length_km, 0.0), max_km);
    const float dt = length_km / float(steps);
    const float cos_theta = dot(direction, s.sun.xyz);
    const float phase_r = rayleigh_phase(cos_theta);
    const float phase_m = mie_phase(cos_theta, s.atmosphere.mie_absorption.w);
    // The sun lights the part of each step outside the planet's shadow (as planet_in_scatter).
    const float altitude = r - s.atmosphere.planet.x;
    const float2 shadow = sphere_shadow_span(origin, direction, altitude * (r + s.atmosphere.planet.x), s.sun.xyz);
    luminance = float3(0.0, 0.0, 0.0);
    transmittance = float3(1.0, 1.0, 1.0);
    for (uint k = 0; k < steps; ++k) {
        const float3 p = origin + direction * ((float(k) + 0.5) * dt);
        const Medium m = medium_at(s.atmosphere, length(p) - s.atmosphere.planet.x);
        const float3 step_transmittance = exp(-m.extinction * dt);
        float middle;
        const float lit = lit_fraction(shadow, float(k) * dt, float(k + 1) * dt, middle);
        const float3 q = origin + direction * middle;
        const float3 sun = lit > 0.0 ? sample_transmittance(s, length(q), dot(normalize(q), s.sun.xyz)) * lit :
                                       float3(0.0, 0.0, 0.0);
        const float3 single = (m.rayleigh * phase_r + m.mie * phase_m) * sun;
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
    const float2 uv = (float2(id.xy) + 0.5) / float2(lut_params.size);
    float3 luminance;
    float3 transmittance;
    if (s.planet != 0) {  // every sun, the ground, around the planet's own up
        planet_march(s, s.origin.xyz, max(s.origin.w, 0.0), planet_sky_view_direction(s, uv), 1e9, s.march_steps, true,
                     luminance, transmittance);
    } else {
        integrate_scattering(s, camera_origin(s), sky_view_direction(s, uv), 1e9, 30, luminance, transmittance);
    }
    lut_params.output[id.xy] = float4(luminance, 1.0);
}

// ---- the ambient table (veach/ambient.slang, ADR 0095) ------------------------------------------------------------

/// Each thread's partial sums, coefficient-major. float4 in one dimension: a 2D threadgroup array of float3 read back
/// other threads' sums nondeterministically on Metal (Bistro's ambient light varied by up to FLIP 0.04 between runs).
groupshared float4 ambient_sums[6 * 64];

/// One entry of the medium's ambient table per group of 64 threads: the L2 SH of the radiance around a point at the
/// group's altitude under a sun of color 1 at the group's zenith cosine, every direction through planet_march (the
/// analytic ground lit below the horizon). 256 directions over the half sphere on the sun's side of its plane (the
/// other half mirrors it), 4 per thread, in equal-area cells (32 bands of height by 8 of azimuth), 24 steps each
/// (within FLIP 0.002 of 2,048 directions in Bistro). Writes block b of the entry at (b * angles + x, y).
[shader("compute")]
[numthreads(64, 1, 1)]
void sky_ambient_lut(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID) {
    const SkyFrame s0 = lut_params.sky[0];
    const float bottom = s0.atmosphere.planet.x;
    const float thickness = s0.atmosphere.planet.y - bottom;
    const float mu = -1.0 + 2.0 * float(group.x) / float(ambient_table_angles - 1);
    const float t = float(group.y) / float(ambient_table_heights - 1);
    const float altitude = max(t * t * thickness, 0.01);
    // The entry's frame: up is +Y, the sun in the XY plane toward +X; Z is across the sun's plane.
    SkyFrame s = s0;
    s.sun_count = 1;
    s.sun_directions[0] = float4(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0, sun_angular_radius);
    s.sun_colors[0] = float4(1.0, 1.0, 1.0, 0.0);
    s.occluder_count = 0;
    s.ground = float4(s0.planet != 0 ? s0.ground.xyz : s0.atmosphere.planet.zzz, 0.0);
    const float3 origin = float3(0.0, bottom + altitude, 0.0);
    float3 sums[6] = { float3(0.0), float3(0.0), float3(0.0), float3(0.0), float3(0.0), float3(0.0) };
    for (uint k = 0; k < 4; ++k) {
        const uint index = thread.x + 64 * k;
        const float up = 1.0 - 2.0 * (float(index / 8) + 0.5) / 32.0;
        const float phi = atmosphere_pi * (float(index % 8) + 0.5) / 8.0;
        const float side = sqrt(max(1.0 - up * up, 0.0));
        const float3 direction = float3(side * cos(phi), up, side * sin(phi));
        float3 luminance;
        float3 transmittance;
        planet_march(s, origin, altitude, direction, 1e9, 24, true, luminance, transmittance);
        float y[6];
        ambient_basis(float3(direction.x, direction.z, direction.y), y);
        for (uint i = 0; i < 6; ++i) {
            sums[i] += luminance * y[i];
        }
    }
    for (uint i = 0; i < 6; ++i) {
        ambient_sums[i * 64 + thread.x] = float4(sums[i], 0.0);
    }
    GroupMemoryBarrierWithGroupSync();
    if (thread.x < 6) {
        float3 total = float3(0.0, 0.0, 0.0);
        for (uint j = 0; j < 64; ++j) {
            total += ambient_sums[thread.x * 64 + j].xyz;
        }
        // Each cell is 4 pi / 512 of the sphere and stands for its mirror image too.
        lut_params.output[uint2(thread.x * ambient_table_angles + group.x, group.y)] =
            float4(total * (8.0 * atmosphere_pi / 512.0), 1.0);
    }
}
