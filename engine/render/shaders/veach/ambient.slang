// The real-time renderer's ambient light (ADR 0095, docs/specs/ambient-light/spec.md): the sky's light on a surface,
// diffuse and specular, as the air around that surface gives it, times its ambient occlusion. The one function the
// shading calls is ambient_light().
//
// The sky's radiance around a point in an atmosphere depends on the medium, the point's altitude and the sun's zenith
// angle there, and is symmetric about the sun's vertical plane. The ambient table holds, per medium, its L2 spherical
// harmonics (radiance per lux of a sun of color 1, the analytic ground lit below the horizon) for 64 sun angles and 16
// altitudes, in each point's own frame: up, the sun's horizontal direction, and their perpendicular. The 3 of 9
// coefficients that are odd across the sun's plane vanish, so each entry is 6 rgb coefficients. A surface reads the
// entry of its own altitude and sun angle for every sun, so the night side gets the night's light wherever the camera
// is. Built by atmosphere.slang's sky_ambient_lut when the medium or its ground changes.

import veach.atmosphere;

static const uint ambient_table_angles = 64;   ///< sun zenith cosines, -1 to 1 (render::ambient_table_angles)
static const uint ambient_table_heights = 16;  ///< altitudes, 0 to the top as t^2 (render::ambient_table_heights)
static const uint ambient_table_blocks = 6;    ///< coefficient blocks side by side, each angles x heights

/// L2 SH coefficients of the radiance around a point, in its frame (a: toward the sun's azimuth, b: across it, c: up).
struct AmbientSh {
    float3 c[6];  ///< Y00, Y10 (c), Y11 (a), Y20, Y21 (ac), Y22 (a^2 - b^2)
};

/// The 6 basis functions that are even in b, at unit direction (a, b, c).
void ambient_basis(float3 d, out float y[6]) {
    y[0] = 0.282095;
    y[1] = 0.488603 * d.z;
    y[2] = 0.488603 * d.x;
    y[3] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    y[4] = 1.092548 * d.x * d.z;
    y[5] = 0.546274 * (d.x * d.x - d.y * d.y);
}

/// The SH band of each coefficient.
static const uint ambient_band[6] = { 0, 1, 1, 2, 2, 2 };

/// The radiance convolved with a lobe whose band attenuation is `k` (band 0, 1, 2), toward `d` (point frame).
float3 ambient_sh_eval(AmbientSh sh, float3 d, float3 k) {
    float y[6];
    ambient_basis(d, y);
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0; i < 6; ++i) {
        sum += sh.c[i] * (y[i] * k[ambient_band[i]]);
    }
    return max(sum, float3(0.0, 0.0, 0.0));
}

/// The table's entry for sun zenith cosine `mu` and altitude fraction `t` (sqrt of altitude over the shell's
/// thickness), bilinear between entries. rgba32_float, as the atmosphere's luminance tables (ADR 0094): half floats
/// lose deep twilight. The coordinates stay on texel centers inside each block, so blocks never blend.
AmbientSh ambient_sh_at(SkyFrame s, float mu, float t) {
    const float x = clamp((mu * 0.5 + 0.5) * float(ambient_table_angles - 1), 0.0, float(ambient_table_angles - 1));
    const float y = clamp(t * float(ambient_table_heights - 1), 0.0, float(ambient_table_heights - 1));
    const float width = float(ambient_table_angles * ambient_table_blocks);
    AmbientSh sh;
    [unroll]
    for (uint b = 0; b < 6; ++b) {
        const float2 uv = float2((float(b * ambient_table_angles) + x + 0.5) / width,
                                 (y + 0.5) / float(ambient_table_heights));
        sh.c[b] = s.ambient.SampleLevel(s.linear_clamp, uv, 0.0).rgb;
    }
    return sh;
}

/// Band attenuation of a cosine lobe, divided by pi: the convolution gives irradiance / pi, the radiance a white
/// Lambertian surface reflects.
static const float3 ambient_cosine_lobe = float3(1.0, 2.0 / 3.0, 0.25);

/// Band attenuation of the GGX lobe of roughness alpha around its dominant direction: a Gaussian on the sphere whose
/// variance grows as 4 alpha^2 (the reflected lobe is twice the half-vector's), never wider than the cosine lobe.
float3 ambient_specular_lobe(float alpha) {
    const float g = min(2.0 * alpha * alpha, 0.2027);  // 0.2027: band 1 at 2/3, the cosine lobe's
    return float3(1.0, exp(-2.0 * g), exp(-6.0 * g));
}

/// The sky's diffuse (irradiance / pi) and specular (pre-filtered radiance toward `dominant`) light at `position`
/// (render space, meters) for a physical sky, in nits, before occlusion. Each sun contributes its own entry, scaled by
/// its color and the fraction of its disk the occluders leave there.
void sky_ambient_at(SkyFrame s, float3 position, float3 camera, float3 normal, float3 dominant, float alpha,
                    out float3 diffuse, out float3 specular) {
    diffuse = float3(0.0, 0.0, 0.0);
    specular = float3(0.0, 0.0, 0.0);
    const float bottom = s.atmosphere.planet.x;
    const float thickness = s.atmosphere.planet.y - bottom;
    const float3 p = s.origin.xyz + (position - camera) * 0.001;  // km from the planet's center
    const float r = length(p);
    const float3 up = p / max(r, 1e-6);
    const float t = sqrt(saturate((r - bottom) / thickness));
    const float3 lobe = ambient_specular_lobe(alpha);
    for (uint i = 0; i < s.sun_count; ++i) {
        const float3 l = s.sun_directions[i].xyz;
        const float mu = dot(up, l);
        float3 a = l - up * mu;
        const float a_length = length(a);
        a = a_length > 1e-5 ? a / a_length : (abs(up.x) < 0.9 ? normalize(cross(up, float3(1.0, 0.0, 0.0)))
                                                                 : normalize(cross(up, float3(0.0, 0.0, 1.0))));
        const float3 b = cross(up, a);
        float3 color = s.sun_colors[i].xyz;
        if (s.occluder_count > 0) {
            color *= planet_sun_visible(s, p, i);
        }
        const AmbientSh sh = ambient_sh_at(s, mu, t);
        diffuse += color * ambient_sh_eval(sh, float3(dot(normal, a), dot(normal, b), dot(normal, up)),
                                           ambient_cosine_lobe);
        specular += color * ambient_sh_eval(sh, float3(dot(dominant, a), dot(dominant, b), dot(dominant, up)), lobe);
    }
    diffuse *= s.sun.w;
    specular *= s.sun.w;
}

/// Karis 2014's fit of the split-sum GGX environment BRDF: specular albedo = f0 * x + y.
float2 ambient_env_brdf(float roughness, float n_dot_v) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

/// Jimenez et al. 2016: visibility with light bouncing between the occluders of albedo `albedo`.
float3 ambient_multibounce(float visibility, float3 albedo) {
    const float3 a = 2.0404 * albedo - 0.3324;
    const float3 b = -4.7951 * albedo + 0.6417;
    const float3 c = 2.7552 * albedo + 0.6903;
    return max(float3(visibility, visibility, visibility), ((visibility * a + b) * visibility + c) * visibility);
}

/// Lagarde and de Rousiers 2014: the occlusion of the specular lobe from ambient occlusion.
float ambient_specular_occlusion(float n_dot_v, float occlusion, float roughness) {
    return saturate(pow(n_dot_v + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion);
}

/// What the ambient light needs to know about a surface.
struct AmbientSurface {
    float3 position;   ///< render space, meters
    float3 normal;     ///< the shading normal, facing the viewer
    float3 view;       ///< toward the camera
    float3 diffuse;    ///< Brdf.diffuse: base color x (1 - metallic)
    float3 f0;         ///< Brdf.f0
    float roughness;   ///< perceptual
    float occlusion;   ///< ambient occlusion: 1 open, 0 closed (ambient_occlusion.slang)
    float distance;    ///< meters from the camera, along the view ray
};

/// The ambient light leaving `surface` toward the camera (nits): the sky's diffuse light on its normal and its
/// specular light along its reflection, occluded by its ambient occlusion (with interreflection) and near the camera
/// by the camera's sky visibility (ADR 0092). A constant sky is uniform radiance; no sky gives none.
float3 ambient_light(SkyFrame s, float3 camera, AmbientSurface surface) {
    if (s.mode != sky_physical && s.mode != sky_constant) {
        return float3(0.0, 0.0, 0.0);
    }
    const float n_dot_v = saturate(dot(surface.normal, surface.view));
    const float alpha = surface.roughness * surface.roughness;
    const float2 env = ambient_env_brdf(surface.roughness, n_dot_v);
    const float3 specular_albedo = surface.f0 * env.x + env.y;
    float3 diffuse;
    float3 specular;
    if (s.mode == sky_constant) {
        diffuse = s.constant.xyz;
        specular = s.constant.xyz;
    } else {
        // The dominant direction of the GGX lobe leans from the mirror direction to the normal as it widens
        // (Lagarde and de Rousiers 2014).
        const float3 mirror = reflect(-surface.view, surface.normal);
        const float lean = (1.0 - alpha) * (sqrt(1.0 - alpha) + alpha);
        const float3 dominant = normalize(lerp(surface.normal, mirror, lean));
        sky_ambient_at(s, surface.position, camera, surface.normal, dominant, alpha, diffuse, specular);
    }
    const float diffuse_visibility = surface.occlusion;
    const float specular_visibility = ambient_specular_occlusion(n_dot_v, surface.occlusion, surface.roughness);
    const float3 light = diffuse * surface.diffuse * (1.0 - specular_albedo) *
                             ambient_multibounce(diffuse_visibility, surface.diffuse) +
                         specular * specular_albedo * ambient_multibounce(specular_visibility, surface.f0);
    return light * sky_visibility_at(s, surface.distance * 0.001);
}
