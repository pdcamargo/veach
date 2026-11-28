// The real-time renderer's ambient light (ADR 0096, docs/specs/ambient-light/spec.md): the sky's light on a surface,
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
//
// The table is the open sky over open, sunlit ground. The large-scale sky occlusion (veach/sky_occlusion.slang) takes
// out of it, per sun, the sky the scene hides from the point (in each of 8 azimuths, the table's radiance toward the
// middle of what is hidden there) and puts in its place the surroundings' bounce (the table's ground, times the share
// of them in that sun); the ground's own bounce is scaled by the share of the ground in the sun.

import veach.atmosphere;
import veach.sky_occlusion;

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

/// One sun's share of the ambient table at a point: the point's frame for that sun (a: toward the sun's azimuth, b:
/// across it, up), the sun's color with the fraction of its disk the occluders leave, and the table's entry.
struct AmbientSun {
    float3 a;
    float3 b;
    float3 up;
    float3 color;
    AmbientSh sh;
};

/// Sun `i` of the physical sky at `position` (render space, meters).
AmbientSun ambient_sun(SkyFrame s, float3 position, float3 camera, uint i) {
    const float bottom = s.atmosphere.planet.x;
    const float thickness = s.atmosphere.planet.y - bottom;
    const float3 p = s.origin.xyz + (position - camera) * 0.001;  // km from the planet's center
    const float r = length(p);
    AmbientSun sun;
    sun.up = p / max(r, 1e-6);
    const float t = sqrt(saturate((r - bottom) / thickness));
    const float3 l = s.sun_directions[i].xyz;
    const float mu = dot(sun.up, l);
    const float3 a = l - sun.up * mu;
    const float a_length = length(a);
    sun.a = a_length > 1e-5 ? a / a_length
                            : (abs(sun.up.x) < 0.9 ? normalize(cross(sun.up, float3(1.0, 0.0, 0.0)))
                                                   : normalize(cross(sun.up, float3(0.0, 0.0, 1.0))));
    sun.b = cross(sun.up, sun.a);
    sun.color = s.sun_colors[i].xyz * s.sun.w;
    if (s.occluder_count > 0) {
        sun.color *= planet_sun_visible(s, p, i);
    }
    sun.sh = ambient_sh_at(s, mu, t);
    return sun;
}

/// The table's radiance around `sun`'s point toward world direction `d`, convolved with band attenuations `k`.
float3 ambient_sun_eval(AmbientSun sun, float3 d, float3 k) {
    return ambient_sh_eval(sun.sh, float3(dot(d, sun.a), dot(d, sun.b), dot(d, sun.up)), k);
}

/// A pair of sector parts (veach/sky_occlusion.slang's SkyOcclusionTerms): the table's radiance toward each part's middle
/// elevation in azimuth k, times its weight.
float3 sky_pair(AmbientSun sun, SkyOcclusionTerms o, uint k, float4 w) {
    float3 sum = float3(0.0, 0.0, 0.0);
    if (w.x > 0.0) {
        sum += w.x * ambient_sun_eval(sun, sky_sector_direction(o, k, w.y), sky_sector_lobe);
    }
    if (w.z > 0.0) {
        sum += w.z * ambient_sun_eval(sun, sky_sector_direction(o, k, w.w), sky_sector_lobe);
    }
    return sum;
}

/// The sky's diffuse (irradiance / pi) and specular (pre-filtered radiance toward `dominant`) light at `position`
/// (render space, meters) for a physical sky, in nits, before any occlusion. Each sun contributes its own entry,
/// scaled by its color and the fraction of its disk the occluders leave there.
void sky_ambient_at(SkyFrame s, float3 position, float3 camera, float3 normal, float3 dominant, float alpha,
                    out float3 diffuse, out float3 specular) {
    diffuse = float3(0.0, 0.0, 0.0);
    specular = float3(0.0, 0.0, 0.0);
    const float3 lobe = ambient_specular_lobe(alpha);
    for (uint i = 0; i < s.sun_count; ++i) {
        const AmbientSun sun = ambient_sun(s, position, camera, i);
        diffuse += sun.color * ambient_sun_eval(sun, normal, ambient_cosine_lobe);
        specular += sun.color * ambient_sun_eval(sun, dominant, lobe);
    }
}

/// The sky's light with the large-scale sky occlusion `o` (veach/sky_occlusion.slang), in `diffuse` and `specular`,
/// and without it, in `open_diffuse` and `open_specular`, the diffuse both by the same quadrature over 8 azimuths'
/// sectors (so their ratio is exact where nothing is hidden and 0 where all is). Per sun: the sky the scene leaves
/// visible; in place of what it hides, the surroundings' bounce (the table's sunlit ground where what hides it is in
/// the sun, the sky's light on it where it is in shade); the ground the same way.
void sky_ambient_occluded(SkyFrame s, float3 position, float3 camera, float3 dominant, float alpha,
                          SkyOcclusionTerms o, out float3 open_diffuse, out float3 open_specular, out float3 diffuse,
                          out float3 specular) {
    open_diffuse = float3(0.0, 0.0, 0.0);
    open_specular = float3(0.0, 0.0, 0.0);
    diffuse = float3(0.0, 0.0, 0.0);
    specular = float3(0.0, 0.0, 0.0);
    const float3 lobe = ambient_specular_lobe(alpha);
    for (uint i = 0; i < s.sun_count; ++i) {
        const AmbientSun sun = ambient_sun(s, position, camera, i);
        const float3 r = ambient_sun_eval(sun, dominant, lobe);
        const uint slot = sky_light_slot(o, s.sun_directions[i].xyz);
        const float lit_blocked = slot < 2 ? o.lit_blocked[slot] : o.sun_visible[min(i, 3u)];
        const float lit_below = slot < 2 ? o.lit_below[slot] : o.sun_visible[min(i, 3u)];
        const float3 ground = ambient_sun_eval(sun, -sun.up, ambient_cosine_lobe);
        // What the surroundings in shade reflect: the sky's light on them (half of an upward face's, they are walls
        // and ground among walls) times the ground's albedo. The table's ground is lit by the sun alone.
        const float3 albedo = s.planet != 0 ? s.ground.xyz : s.atmosphere.planet.zzz;
        const float3 shade = 0.5 * albedo * ambient_sun_eval(sun, sun.up, ambient_cosine_lobe);
        float3 sky = float3(0.0, 0.0, 0.0);
        float3 visible = float3(0.0, 0.0, 0.0);
        float3 below = float3(0.0, 0.0, 0.0);
        for (uint k = 0; k < sky_azimuths; ++k) {
            sky += sky_pair(sun, o, k, o.sky[k]);
            visible += sky_pair(sun, o, k, o.visible[k]);
            below += sky_pair(sun, o, k, o.ground[k]);
        }
        open_diffuse += sun.color * (sky + below);
        diffuse += sun.color * (visible + o.blocked * lerp(shade, ground, lit_blocked) + below * lit_below +
                                o.below * shade * (1.0 - lit_below));
        open_specular += sun.color * r;
        specular += sun.color * max(r * (1.0 - o.specular_blocked - o.specular_below * (1.0 - lit_below)) +
                                        o.specular_blocked * lerp(shade, ground, lit_blocked) +
                                        o.specular_below * shade * (1.0 - lit_below),
                                    float3(0.0, 0.0, 0.0));
    }
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
    SkyOcclusionSample sky;  ///< the large-scale sky occlusion here (sky_occlusion_at; sky_occlusion_open: none)
};

/// The ambient light leaving `surface` toward the camera (nits): the sky's diffuse light on its normal and its
/// specular light along its reflection, less what the scene hides from it at large scale (veach/sky_occlusion.slang),
/// occluded by its ambient occlusion (with interreflection) and, where the large-scale occlusion does not reach, near
/// the camera by the camera's sky visibility (ADR 0092). A constant sky is uniform radiance; no sky gives none.
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
    // The dominant direction of the GGX lobe leans from the mirror direction to the normal as it widens (Lagarde and
    // de Rousiers 2014).
    const float3 mirror = reflect(-surface.view, surface.normal);
    const float lean = (1.0 - alpha) * (sqrt(1.0 - alpha) + alpha);
    const float3 dominant = normalize(lerp(surface.normal, mirror, lean));
    if (s.mode == sky_constant) {
        diffuse = s.constant.xyz;
        specular = s.constant.xyz;
    } else {
        sky_ambient_at(s, surface.position, camera, surface.normal, dominant, alpha, diffuse, specular);
    }
    // The large-scale sky occlusion: the share of this light the scene leaves (sky_occlusion_at).
    diffuse *= surface.sky.diffuse;
    specular *= surface.sky.specular;
    const float diffuse_visibility = surface.occlusion;
    const float specular_visibility = ambient_specular_occlusion(n_dot_v, surface.occlusion, surface.roughness);
    const float3 light = diffuse * surface.diffuse * (1.0 - specular_albedo) *
                             ambient_multibounce(diffuse_visibility, surface.diffuse) +
                         specular * specular_albedo * ambient_multibounce(specular_visibility, surface.f0);
    // The camera's sky visibility stands in for the scene where the large-scale occlusion does not reach.
    return light * lerp(sky_visibility_at(s, surface.distance * 0.001), 1.0, surface.sky.fade);
}
