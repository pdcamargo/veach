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
    float4 rayleigh;  ///< xyz scattering (1/km), w = scale height (km)
    float4 mie;       ///< x scattering, y absorption (1/km), z = scale height (km), w = phase g
    float4 ozone;     ///< xyz absorption (1/km), w = tent center (km)
    float4 planet;    ///< x = ground radius, y = top radius (km), z = ground albedo, w = ozone tent half-width (km)
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
};

// ---- the medium ----------------------------------------------------------------------------------------------

struct Medium {
    float3 scattering;  ///< Rayleigh + Mie
    float3 extinction;
    float3 rayleigh;
    float mie;
};

Medium medium_at(AtmosphereParams a, float height) {
    const float rayleigh_density = exp(-height / a.rayleigh.w);
    const float mie_density = exp(-height / a.mie.z);
    const float ozone_density = max(0.0, 1.0 - abs(height - a.ozone.w) / a.planet.w);
    Medium m;
    m.rayleigh = a.rayleigh.xyz * rayleigh_density;
    m.mie = a.mie.x * mie_density;
    m.scattering = m.rayleigh + m.mie;
    m.extinction = m.scattering + a.mie.y * mie_density + a.ozone.xyz * ozone_density;
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
