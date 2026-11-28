// The physical atmosphere (ADR 0038, docs/specs/physical-sky/spec.md), after Hillaire 2020: Rayleigh and Mie
// scattering and ozone absorption over a spherical planet, tabulated in lookup tables that both renderers read.
// Lengths in kilometers. LUT luminance is per lux of sun illuminance at the top of the atmosphere; SkyFrame scales it
// to nits. The CPU mirror (render/src/atmosphere.cpp) integrates transmittance the same way.

import veach.bindless;

static const float atmosphere_pi = 3.14159265358979;
static const uint sky_none = 0;
static const uint sky_constant = 1;
static const uint sky_physical = 2;

/// Mirrors render::AtmosphereParams: the medium.
struct AtmosphereParams {
    float4 rayleigh;        ///< xyz scattering (1/km), w = scale height (km)
    float4 mie;             ///< xyz scattering (1/km), w = scale height (km)
    float4 mie_absorption;  ///< xyz absorption (1/km), w = phase g
    float4 ozone;           ///< xyz absorption (1/km), w = tent center (km)
    float4 planet;          ///< x = ground radius, y = top radius (km), z = ground albedo, w = ozone tent half-width (km)
};

/// Mirrors render::SkyFrame: everything a pass needs about this frame's sky, in one structured-buffer element.
struct SkyFrame {
    AtmosphereParams atmosphere;
    float4 sun;        ///< xyz direction toward the sun (world, +Y up), w = sun illuminance at the top (lux)
    float4 camera;     ///< x = camera altitude above the ground (km), yzw unused
    float4 constant;   ///< xyz = Sky.color (nits) for constant mode
    float4x4 inverse_view_projection;  ///< unjittered: the aerial-perspective volume's frustum
    DescriptorHandle<Texture2D<float4>> transmittance;
    DescriptorHandle<Texture2D<float4>> multiscattering;
    DescriptorHandle<Texture2D<float4>> sky_view;
    DescriptorHandle<Texture2D<float4>> aerial;      ///< 32 slices of 32 x 32, side by side
    DescriptorHandle<Texture2D<float4>> irradiance;  ///< 1 x 1: the sky's average for an upward normal
    DescriptorHandle<SamplerState> linear_clamp;
    uint mode;         ///< sky_*
    uint aerial_on;    ///< 1: apply aerial perspective
    uint pad0;
    uint pad1;
    // Spike SP-10 (VCH-086), throwaway: a planet around any center, up to four suns, one spherical occluder.
    // Read only when `planet` is 1; the Sky path above ignores them.
    float4 origin;           ///< xyz = the camera relative to the planet's center (km), w = its altitude (km)
    float4 frame_x;          ///< xyz = the sky-view LUT's azimuth origin: a unit vector perpendicular to local up
    float4 ground;           ///< xyz = the analytic ground's albedo (rgb)
    float4 occluder;         ///< xyz = center relative to the planet's center (km), w = radius (km; 0: none)
    float4 occluder_albedo;  ///< xyz
    float4 sun_directions[4];  ///< xyz toward the sun, w = angular radius (radians)
    float4 sun_colors[4];      ///< xyz = illuminance at the top (lux, rgb) / sun.w
    uint planet;       ///< 1: planet mode
    uint sun_count;
    uint march;        ///< 0: the sky-view LUT inside the atmosphere, a per-pixel march outside; 1: always march; 2: never
    uint march_steps;  ///< steps of the per-pixel march and the sky-view LUT
};

// ---- the medium ----------------------------------------------------------------------------------------------

struct Medium {
    float3 scattering;  ///< Rayleigh + Mie
    float3 extinction;
    float3 rayleigh;
    float3 mie;
};

Medium medium_at(AtmosphereParams a, float height) {
    const float rayleigh_density = exp(-height / a.rayleigh.w);
    const float mie_density = exp(-height / a.mie.w);
    const float ozone_density = max(0.0, 1.0 - abs(height - a.ozone.w) / a.planet.w);
    Medium m;
    m.rayleigh = a.rayleigh.xyz * rayleigh_density;
    m.mie = a.mie.xyz * mie_density;
    m.scattering = m.rayleigh + m.mie;
    m.extinction = m.scattering + a.mie_absorption.xyz * mie_density + a.ozone.xyz * ozone_density;
    return m;
}

float rayleigh_phase(float cos_theta) {
    return 3.0 / (16.0 * atmosphere_pi) * (1.0 + cos_theta * cos_theta);
}

/// Cornette-Shanks, as Hillaire.
float mie_phase(float cos_theta, float g) {
    const float k = 3.0 / (8.0 * atmosphere_pi) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cos_theta * cos_theta) / pow(max(1.0 + g * g - 2.0 * g * cos_theta, 1e-6), 1.5);
}

/// Distance along a ray from `origin` (relative to the planet's center) to a sphere of `radius`: the nearest positive
/// hit, or -1.
float ray_sphere(float3 origin, float3 direction, float radius) {
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0) {
        return -1.0;
    }
    const float s = sqrt(discriminant);
    const float t0 = -b - s;
    const float t1 = -b + s;
    return t0 >= 0.0 ? t0 : (t1 >= 0.0 ? t1 : -1.0);
}

// ---- transmittance LUT parameterization (Bruneton 2017, as Hillaire) --------------------------------------------

float2 transmittance_uv(AtmosphereParams a, float r, float mu) {
    const float bottom = a.planet.x;
    const float top = a.planet.y;
    const float h = sqrt(max(top * top - bottom * bottom, 0.0));
    const float rho = sqrt(max(r * r - bottom * bottom, 0.0));
    const float discriminant = r * r * (mu * mu - 1.0) + top * top;
    const float d = max(0.0, -r * mu + sqrt(max(discriminant, 0.0)));
    const float d_min = top - r;
    const float d_max = rho + h;
    return float2((d - d_min) / max(d_max - d_min, 1e-6), rho / h);
}

void transmittance_r_mu(AtmosphereParams a, float2 uv, out float r, out float mu) {
    const float bottom = a.planet.x;
    const float top = a.planet.y;
    const float h = sqrt(top * top - bottom * bottom);
    const float rho = h * uv.y;
    r = sqrt(rho * rho + bottom * bottom);
    const float d_min = top - r;
    const float d_max = rho + h;
    const float d = d_min + uv.x * (d_max - d_min);
    mu = d == 0.0 ? 1.0 : clamp((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

/// Transmittance from radius r toward zenith cosine mu, to the top of the atmosphere (40 steps; the CPU does the same).
float3 integrate_transmittance(AtmosphereParams a, float r, float mu) {
    const float3 origin = float3(0.0, r, 0.0);
    const float3 direction = float3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
    const float length_to_top = ray_sphere(origin, direction, a.planet.y);
    if (length_to_top <= 0.0) {
        return float3(1.0, 1.0, 1.0);
    }
    const uint steps = 40;
    const float dt = length_to_top / float(steps);
    float3 depth = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < steps; ++i) {
        const float3 p = origin + direction * ((float(i) + 0.5) * dt);
        depth += medium_at(a, length(p) - a.planet.x).extinction * dt;
    }
    return exp(-depth);
}

float3 sample_transmittance(SkyFrame s, float r, float mu) {
    return s.transmittance.SampleLevel(s.linear_clamp, transmittance_uv(s.atmosphere, r, mu), 0.0).rgb;
}

// ---- sky-view LUT parameterization (Hillaire: latitude squeezed toward the horizon) ----------------------------

/// uv of a view direction: x = azimuth relative to the sun's, y = elevation (non-linear around the horizon).
float2 sky_view_uv(SkyFrame s, float3 direction) {
    const float r = s.atmosphere.planet.x + s.camera.x;
    const float horizon = acos(clamp(-sqrt(max(r * r - s.atmosphere.planet.x * s.atmosphere.planet.x, 0.0)) / r, -1.0, 1.0));
    const float zenith = acos(clamp(direction.y, -1.0, 1.0));  // 0 up
    const float horizon_elevation = atmosphere_pi * 0.5 - horizon;  // negative: below the geometric horizon
    const float elevation = atmosphere_pi * 0.5 - zenith;
    float v;
    if (elevation >= horizon_elevation) {
        const float t = (elevation - horizon_elevation) / (atmosphere_pi * 0.5 - horizon_elevation);
        v = 0.5 + 0.5 * sqrt(saturate(t));
    } else {
        const float t = (horizon_elevation - elevation) / (horizon_elevation + atmosphere_pi * 0.5);
        v = 0.5 - 0.5 * sqrt(saturate(t));
    }
    const float view_azimuth = atan2(direction.z, direction.x);
    const float sun_azimuth = atan2(s.sun.z, s.sun.x);
    float delta = view_azimuth - sun_azimuth;
    delta = delta - 2.0 * atmosphere_pi * floor((delta + atmosphere_pi) / (2.0 * atmosphere_pi));  // -pi..pi
    return float2(0.5 + delta / (2.0 * atmosphere_pi), v);
}

/// The view direction of a sky-view texel (inverse of sky_view_uv), world space.
float3 sky_view_direction(SkyFrame s, float2 uv) {
    const float r = s.atmosphere.planet.x + s.camera.x;
    const float horizon = acos(clamp(-sqrt(max(r * r - s.atmosphere.planet.x * s.atmosphere.planet.x, 0.0)) / r, -1.0, 1.0));
    const float horizon_elevation = atmosphere_pi * 0.5 - horizon;
    float elevation;
    if (uv.y >= 0.5) {
        const float t = (uv.y - 0.5) * 2.0;
        elevation = horizon_elevation + t * t * (atmosphere_pi * 0.5 - horizon_elevation);
    } else {
        const float t = (0.5 - uv.y) * 2.0;
        elevation = horizon_elevation - t * t * (horizon_elevation + atmosphere_pi * 0.5);
    }
    const float azimuth = atan2(s.sun.z, s.sun.x) + (uv.x - 0.5) * 2.0 * atmosphere_pi;
    return float3(cos(elevation) * cos(azimuth), sin(elevation), cos(elevation) * sin(azimuth));
}

// ---- what the renderers read -------------------------------------------------------------------------------------

static const float sun_angular_radius = 0.004625;  ///< 0.53 degrees across

/// The sky's radiance toward `direction` (nits); `with_sun` adds the sun's disk (camera rays only: elsewhere the sun
/// is the directional light).
float3 sky_radiance(SkyFrame s, float3 direction, bool with_sun) {
    if (s.mode == sky_none) {
        return float3(0.0, 0.0, 0.0);
    }
    if (s.mode == sky_constant) {
        return s.constant.xyz;
    }
    float3 l = s.sky_view.SampleLevel(s.linear_clamp, sky_view_uv(s, direction), 0.0).rgb;
    if (with_sun && dot(direction, s.sun.xyz) > cos(sun_angular_radius)) {
        const float r = s.atmosphere.planet.x + s.camera.x;
        const float solid_angle = 2.0 * atmosphere_pi * (1.0 - cos(sun_angular_radius));
        const float ground = ray_sphere(float3(0.0, r, 0.0), direction, s.atmosphere.planet.x);
        if (ground < 0.0) {
            l += sample_transmittance(s, r, direction.y) / solid_angle;
        }
    }
    return l * s.sun.w;
}

/// The real-time renderer's ambient radiance over the upper hemisphere (nits): the sky's cosine-weighted average.
float3 sky_ambient_radiance(SkyFrame s) {
    if (s.mode == sky_physical) {
        return s.irradiance.Load(int3(0, 0, 0)).rgb * s.sun.w;
    }
    return s.mode == sky_constant ? s.constant.xyz : float3(0.0, 0.0, 0.0);
}

static const uint aerial_slices = 32;
static const float aerial_depth_km = 32.0;

/// The aerial-perspective slice coordinate (0..1, quadratic: finer near the camera) of a view depth in km.
float aerial_slice(float depth_km) {
    return sqrt(saturate(depth_km / aerial_depth_km));
}

/// Light added and transmittance (in .a) between the camera and a point at `depth_m` meters along the view through
/// screen position `uv` (0..1, y down). Identity when the sky is not physical or aerial perspective is off.
float4 aerial_perspective(SkyFrame s, float2 uv, float depth_m) {
    if (s.mode != sky_physical || s.aerial_on == 0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float slice = aerial_slice(depth_m * 0.001) * float(aerial_slices) - 0.5;
    const float lo = clamp(floor(slice), 0.0, float(aerial_slices - 1));
    const float hi = min(lo + 1.0, float(aerial_slices - 1));
    const float f = saturate(slice - lo);
    const float2 cell = clamp(uv, 0.5 / 32.0, 1.0 - 0.5 / 32.0);
    const float4 a = s.aerial.SampleLevel(s.linear_clamp, float2((lo + cell.x) / float(aerial_slices), cell.y), 0.0);
    const float4 b = s.aerial.SampleLevel(s.linear_clamp, float2((hi + cell.x) / float(aerial_slices), cell.y), 0.0);
    float4 v = lerp(a, b, f);
    if (slice < 0.0) {  // nearer than the first slice's center: fade toward no effect at the camera
        const float w = saturate((slice + 0.5) / 0.5);
        v = lerp(float4(0.0, 0.0, 0.0, 1.0), v, w);
    }
    return float4(v.rgb * s.sun.w, v.a);
}

// ---- spike SP-10 (VCH-086), throwaway: a planet around any center, from the ground to orbit ----------------------
//
// Positions are relative to the planet's center, in km; local up is the direction from the center. All suns are
// summed into one sky-view LUT parameterized by absolute azimuth around local up (from frame_x). The LUT path and the
// per-pixel march call the same integrator (planet_march) with the same step count, so switching between them at the
// top of the atmosphere differs only by the LUT's resolution. Luminance is per lux of `sun.w` (the brightest sun's
// unattenuated illuminance); each sun's own illuminance is sun_colors[i] in those units.

static const uint planet_max_suns = 4;

float3 planet_up(SkyFrame s) {
    return normalize(s.origin.xyz);
}

/// The other horizontal axis of the sky-view LUT's frame (frame_x, up, frame_z is right-handed).
float3 planet_frame_z(SkyFrame s) {
    return cross(s.frame_x.xyz, planet_up(s));
}

/// Where origin + t * direction meets a sphere about the planet's center, given c = |origin|^2 - radius^2 (callers
/// at the camera compute c from the altitude, which does not cancel at planetary radii). t0 <= t1; false if it
/// misses. The stable quadratic: the root near 0 keeps its precision.
bool sphere_span(float3 origin, float3 direction, float c, out float t0, out float t1) {
    const float b = dot(origin, direction);
    const float discriminant = b * b - c;
    t0 = 0.0;
    t1 = 0.0;
    if (discriminant < 0.0) {
        return false;
    }
    const float q = b >= 0.0 ? -b - sqrt(discriminant) : -b + sqrt(discriminant);
    const float other = q != 0.0 ? c / q : 0.0;
    t0 = min(q, other);
    t1 = max(q, other);
    return true;
}

/// The visible fraction of a sun's disk (angular radius `sun_radius`) toward `l` seen from `p`, with the occluder
/// sphere in the way: the area of the sun's disk outside the occluder's, as flat disks (the lens-area formula; the
/// spike's unit test checks the CPU twin against a numeric integral).
float planet_sun_visible(SkyFrame s, float3 p, float3 l, float sun_radius) {
    if (s.occluder.w <= 0.0) {
        return 1.0;
    }
    const float3 d = s.occluder.xyz - p;
    const float distance = length(d);
    if (dot(d, l) <= 0.0) {
        return 1.0;  // the occluder is behind
    }
    if (distance <= s.occluder.w) {
        return 0.0;
    }
    const float a = asin(s.occluder.w / distance);                         // the occluder's angular radius
    const float c = 2.0 * asin(min(length(d / distance - l) * 0.5, 1.0));  // separation, precise at small angles
    const float b = sun_radius;
    if (c >= a + b) {
        return 1.0;
    }
    if (c <= abs(a - b)) {
        return a >= b ? 0.0 : 1.0 - (a * a) / (b * b);
    }
    const float alpha = acos(clamp((c * c + a * a - b * b) / (2.0 * c * a), -1.0, 1.0));
    const float beta = acos(clamp((c * c + b * b - a * a) / (2.0 * c * b), -1.0, 1.0));
    const float lens = a * a * (alpha - 0.5 * sin(2.0 * alpha)) + b * b * (beta - 0.5 * sin(2.0 * beta));
    return saturate(1.0 - lens / (atmosphere_pi * b * b));
}

/// Transmittance of sun `i`'s light to `p` (0 in the planet's shadow); `visible` is the fraction of its disk the
/// occluder leaves.
float3 planet_sun_transmittance(SkyFrame s, float3 p, uint i, out float visible) {
    const float3 l = s.sun_directions[i].xyz;
    visible = planet_sun_visible(s, p, l, s.sun_directions[i].w);
    // The planet's shadow: the ray toward the sun heads inward and passes closer than the radius. Not ray_sphere:
    // seen from orbit, sample points carry meters of rounding, and one rounded under the ground would shadow itself.
    const float b = dot(p, l);
    const float r = length(p);
    if (b < 0.0 && r * r - b * b < s.atmosphere.planet.x * s.atmosphere.planet.x) {
        return float3(0.0, 0.0, 0.0);
    }
    return sample_transmittance(s, r, dot(p / r, l));
}

/// Hillaire's multiple-scattering term at `p` for sun `i` (per lux of that sun).
float3 planet_multiple_scattering(SkyFrame s, float3 p, uint i) {
    const float r = length(p);
    const float mu = dot(p / r, s.sun_directions[i].xyz);
    const float height = (r - s.atmosphere.planet.x) / (s.atmosphere.planet.y - s.atmosphere.planet.x);
    return s.multiscattering.SampleLevel(s.linear_clamp, float2(mu * 0.5 + 0.5, saturate(height)), 0.0).rgb;
}

/// In-scattered luminance (per lux of sun.w) and transmittance along origin + t * direction, t in [0, t_max],
/// clipped to the atmosphere's shell, for every sun. `origin` is the camera; `altitude` its height above the ground
/// (km), so its own intersections do not cancel at planetary radii. With `ground`, the lit analytic ground where the
/// ray meets it is added and the transmittance beyond it is 0.
void planet_march(SkyFrame s, float3 origin, float altitude, float3 direction, float t_max, uint steps, bool ground,
                  out float3 luminance, out float3 transmittance) {
    luminance = float3(0.0, 0.0, 0.0);
    transmittance = float3(1.0, 1.0, 1.0);
    const float bottom = s.atmosphere.planet.x;
    const float top = s.atmosphere.planet.y;
    const float r = bottom + altitude;
    float t0;
    float t1;
    if (!sphere_span(origin, direction, (r - top) * (r + top), t0, t1) || t1 <= 0.0) {
        return;  // misses the atmosphere
    }
    float g0;
    float g1;
    const bool meets_ground = sphere_span(origin, direction, altitude * (r + bottom), g0, g1) && g0 > 0.0;
    const float start = max(t0, 0.0);
    const float end = min(meets_ground ? g0 : t1, t_max);
    if (end <= start) {
        return;
    }
    float phase_r[planet_max_suns];
    float phase_m[planet_max_suns];
    for (uint i = 0; i < s.sun_count; ++i) {
        const float cos_theta = dot(direction, s.sun_directions[i].xyz);
        phase_r[i] = rayleigh_phase(cos_theta);
        phase_m[i] = mie_phase(cos_theta, s.atmosphere.mie_absorption.w);
    }
    const float dt = (end - start) / float(steps);
    for (uint k = 0; k < steps; ++k) {
        const float3 p = origin + direction * (start + (float(k) + 0.5) * dt);
        const Medium m = medium_at(s.atmosphere, length(p) - bottom);
        const float3 step_transmittance = exp(-m.extinction * dt);
        float3 in_scatter = float3(0.0, 0.0, 0.0);
        for (uint i = 0; i < s.sun_count; ++i) {
            float visible;
            const float3 sun = planet_sun_transmittance(s, p, i, visible);
            const float3 single = (m.rayleigh * phase_r[i] + m.mie * phase_m[i]) * sun;
            in_scatter += (single + m.scattering * planet_multiple_scattering(s, p, i)) * s.sun_colors[i].xyz * visible;
        }
        luminance += transmittance * (in_scatter - in_scatter * step_transmittance) / max(m.extinction, 1e-9);
        transmittance *= step_transmittance;
    }
    if (ground && meets_ground && g0 <= t_max) {
        const float3 p = origin + direction * g0;
        const float3 n = normalize(p);
        float3 irradiance = float3(0.0, 0.0, 0.0);
        for (uint i = 0; i < s.sun_count; ++i) {
            float visible;
            const float3 sun = planet_sun_transmittance(s, n * s.atmosphere.planet.x, i, visible);
            irradiance += sun * s.sun_colors[i].xyz * visible * saturate(dot(n, s.sun_directions[i].xyz));
        }
        luminance += transmittance * irradiance * s.ground.xyz / atmosphere_pi;
        transmittance = float3(0.0, 0.0, 0.0);
    }
}

/// The horizon's elevation at the camera (radians, negative: below local horizontal), from its altitude.
float planet_horizon_elevation(SkyFrame s) {
    const float bottom = s.atmosphere.planet.x;
    return -acos(clamp(bottom / (bottom + max(s.origin.w, 0.0)), -1.0, 1.0));
}

/// The sky-view LUT's uv of a world direction (planet mode): x = absolute azimuth from frame_x around local up,
/// y = elevation, squeezed toward the horizon as Hillaire's.
float2 planet_sky_view_uv(SkyFrame s, float3 direction) {
    const float horizon_elevation = planet_horizon_elevation(s);
    const float elevation = asin(clamp(dot(direction, planet_up(s)), -1.0, 1.0));
    float v;
    if (elevation >= horizon_elevation) {
        const float t = (elevation - horizon_elevation) / (atmosphere_pi * 0.5 - horizon_elevation);
        v = 0.5 + 0.5 * sqrt(saturate(t));
    } else {
        const float t = (horizon_elevation - elevation) / (horizon_elevation + atmosphere_pi * 0.5);
        v = 0.5 - 0.5 * sqrt(saturate(t));
    }
    const float azimuth = atan2(dot(direction, planet_frame_z(s)), dot(direction, s.frame_x.xyz));  // -pi..pi
    return float2(0.5 + azimuth / (2.0 * atmosphere_pi), v);
}

/// The world direction of a sky-view texel (inverse of planet_sky_view_uv).
float3 planet_sky_view_direction(SkyFrame s, float2 uv) {
    const float horizon_elevation = planet_horizon_elevation(s);
    float elevation;
    if (uv.y >= 0.5) {
        const float t = (uv.y - 0.5) * 2.0;
        elevation = horizon_elevation + t * t * (atmosphere_pi * 0.5 - horizon_elevation);
    } else {
        const float t = (0.5 - uv.y) * 2.0;
        elevation = horizon_elevation - t * t * (horizon_elevation + atmosphere_pi * 0.5);
    }
    const float azimuth = (uv.x - 0.5) * 2.0 * atmosphere_pi;
    return normalize(s.frame_x.xyz * (cos(elevation) * cos(azimuth)) + planet_up(s) * sin(elevation) +
                     planet_frame_z(s) * (cos(elevation) * sin(azimuth)));
}

/// True when this view uses the per-pixel march rather than the sky-view LUT.
bool planet_marches(SkyFrame s) {
    return s.march == 1 || (s.march == 0 && s.atmosphere.planet.x + s.origin.w >= s.atmosphere.planet.y);
}

/// The sky's radiance toward `direction` in planet mode (nits): the atmosphere (LUT or march) with the ground, the
/// occluder (a sphere lit by the suns, beyond the atmosphere) and, with `with_sun`, the suns' disks.
float3 planet_sky_radiance(SkyFrame s, float3 direction, bool with_sun) {
    const float3 origin = s.origin.xyz;
    const float bottom = s.atmosphere.planet.x;
    const float altitude = max(s.origin.w, 0.0);
    float3 l;
    float3 t;
    if (planet_marches(s)) {
        planet_march(s, origin, altitude, direction, 1e9, s.march_steps, true, l, t);
    } else {
        l = s.sky_view.SampleLevel(s.linear_clamp, planet_sky_view_uv(s, direction), 0.0).rgb;
        float g0;
        float g1;
        const bool ground =
            sphere_span(origin, direction, altitude * (2.0 * bottom + altitude), g0, g1) && g0 > 0.0;
        t = ground ? float3(0.0, 0.0, 0.0) :
                     sample_transmittance(s, bottom + altitude, dot(direction, planet_up(s)));
    }
    if (any(t > 0.0)) {
        // The occluder, if the ray meets it (it is beyond the atmosphere: the in-scattering in front is `l`).
        const float3 to_center = s.occluder.xyz - origin;
        const float tc = dot(to_center, direction);
        const float miss2 = dot(to_center, to_center) - tc * tc;
        if (s.occluder.w > 0.0 && tc > 0.0 && miss2 < s.occluder.w * s.occluder.w) {
            const float3 hit = origin + direction * (tc - sqrt(s.occluder.w * s.occluder.w - miss2));
            const float3 n = normalize(hit - s.occluder.xyz);
            float3 irradiance = float3(0.0, 0.0, 0.0);
            for (uint i = 0; i < s.sun_count; ++i) {
                irradiance += s.sun_colors[i].xyz * saturate(dot(n, s.sun_directions[i].xyz));
            }
            l += t * irradiance * s.occluder_albedo.xyz / atmosphere_pi;
        } else if (with_sun) {
            for (uint i = 0; i < s.sun_count; ++i) {
                const float radius = s.sun_directions[i].w;
                if (dot(direction, s.sun_directions[i].xyz) > cos(radius)) {
                    l += t * s.sun_colors[i].xyz / (2.0 * atmosphere_pi * (1.0 - cos(radius)));
                }
            }
        }
    }
    return l * s.sun.w;
}

/// sky_radiance for the real-time renderer: the planet's sky in planet mode (spike SP-10), the Sky's otherwise.
/// Photo mode keeps calling sky_radiance (the path tracer's kernel size is near Metal's compiler limit).
float3 sky_radiance_any(SkyFrame s, float3 direction, bool with_sun) {
    if (s.planet != 0 && s.mode == sky_physical) {
        return planet_sky_radiance(s, direction, with_sun);
    }
    return sky_radiance(s, direction, with_sun);
}
