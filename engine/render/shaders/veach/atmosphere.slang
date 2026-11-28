// The physical atmosphere (ADR 0038, 0063, 0076, 0082, docs/specs/physical-sky/spec.md and
// docs/specs/planet-atmosphere/spec.md), after Hillaire 2020: Rayleigh and Mie scattering and ozone absorption over a
// spherical planet, tabulated in lookup tables that both renderers read. Lengths in kilometers. LUT luminance is per
// lux of sun illuminance at the top of the atmosphere; SkyFrame scales it to nits. The CPU mirror
// (render/src/atmosphere.cpp) integrates transmittance and intersects spheres the same way.
//
// Two ways in: the Sky component's flat-world sky (planet = 0: the camera above the ground at the origin, +Y up, one
// sun), and a planet around any center (planet = 1: render::PlanetSky, filled by a module's extractor), from the
// ground to orbit, lit by up to four suns, with spherical occluders that eclipse them. The aerial-perspective volume
// and its apply function serve both: the CPU fills the planet block for the Sky too (one sun, centered below the
// camera).

import veach.bindless;

static const float atmosphere_pi = 3.14159265358979;
static const uint sky_none = 0;
static const uint sky_constant = 1;
static const uint sky_physical = 2;
static const uint planet_max_suns = 4;
static const uint planet_max_occluders = 4;
/// The sky-view LUT's size (render::sky_view_size): its elevation is clamped to texel centers so the wrapping sampler
/// wraps azimuth only.
static const float2 sky_view_size = float2(192.0, 108.0);

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
    float4 sun;        ///< xyz direction toward the sun (world), w = the brightest sun's illuminance at the top (lux)
    float4 camera;     ///< x = camera altitude above the ground (km), yzw unused
    float4 constant;   ///< xyz = Sky.color (nits) for constant mode
    float4x4 inverse_view_projection;  ///< unjittered: the aerial-perspective volume's frustum
    DescriptorHandle<Texture2D<float4>> transmittance;
    DescriptorHandle<Texture2D<float4>> multiscattering;
    DescriptorHandle<Texture2D<float4>> sky_view;
    DescriptorHandle<Texture3D<float4>> aerial;      ///< the froxel volume (aerial_volume.slang)
    DescriptorHandle<Texture2D<float4>> irradiance;  ///< 1 x 1: the sky's average for an upward normal
    DescriptorHandle<SamplerState> linear_clamp;
    uint mode;           ///< sky_*
    uint aerial_on;      ///< 1: apply aerial perspective
    uint aerial_slices;  ///< the volume's depth slices
    uint pad1;
    // The planet block. With planet = 1 it is a PlanetSky's; with planet = 0 the CPU fills it for the Sky (one sun of
    // color 1, the center straight below the camera) and only the aerial-perspective volume reads it.
    float4 origin;     ///< xyz = the camera relative to the planet's center (km), w = its altitude (km)
    float4 frame_x;    ///< xyz = the sky-view LUT's azimuth origin: a unit vector perpendicular to local up
    float4 ground;     ///< xyz = the analytic ground's albedo (rgb)
    float4 occluders[planet_max_occluders];        ///< xyz = center relative to the planet's center (km), w = radius
    float4 occluder_albedo[planet_max_occluders];  ///< xyz = albedo, w = 1: the sky draws it (a sphere lit by the suns)
    float4 sun_directions[planet_max_suns];  ///< xyz toward the sun, w = angular radius (radians)
    float4 sun_colors[planet_max_suns];      ///< xyz = illuminance at the top (lux, rgb) / sun.w
    uint planet;       ///< 1: planet mode
    uint sun_count;
    uint march;        ///< 0: the sky-view LUT inside the atmosphere, a per-pixel march outside; 1: always march; 2: never
    uint march_steps;  ///< steps of the per-pixel march and the sky-view LUT
    /// The starfield (render::Starfield). x = brightness (0: off; 1 is physical), y = one pixel's angle (radians),
    /// z = limiting magnitude, w = cells per cube-map face side.
    float4 stars;
    // The space backdrop (veach.backdrop, ADR 0091). Read only when backdrop_info.w is 1.
    float4 galactic_x;      ///< xyz = row 0 of the rotation galactic from render space; w = one pixel's angle (rad)
    float4 galactic_y;      ///< xyz = row 1; w = the creator's exposure compensation (stops)
    float4 galactic_z;      ///< xyz = row 2; w = the creator's exposure follow (0..1)
    float4 camera_right;    ///< xyz = the camera's right, galactic
    float4 camera_up;       ///< xyz = the camera's up, galactic
    float4 backdrop_stars;  ///< x = star density, y = magnitude scale, z = saturation, w = 1 / its band's nits
    uint4 backdrop_info;    ///< x = seed, y = texture width, z = height, w = 1: draw it
    DescriptorHandle<Texture2D<float4>> backdrop;  ///< lat-long, galactic: rgb = nits, a = star density
    DescriptorHandle<SamplerState> linear_repeat;  ///< also the sky-view LUT's: its azimuth wraps
    /// Sky visibility around the camera (render::SkyVisibility, ADR 0082 rule 5): x = the fraction of the sky the
    /// camera's surroundings leave open, y = how far they reach (km). Air and surfaces nearer than that receive the
    /// sky's light times x: caves and interiors get no haze, and it returns beyond them.
    float4 occlusion;
    uint occluder_count;
    uint pad2;
    uint pad3;
    uint pad4;
};

// ---- spheres ---------------------------------------------------------------------------------------------------
//
// One helper for every ray-sphere test (ADR 0076 decision 6). Kilometer floats from orbit at real scale lose the
// near root with the textbook quadratic and shadow sample points that rounding put a few meters under the ground;
// the stable quadratic and the inward-ray shadow test don't. Tested at 1e4 km on the CPU and the GPU
// (tests/atmosphere_tests.cpp); render::sphere_span is the CPU twin.

/// Where origin + t * direction (a unit vector) is inside a sphere about the origin of coordinates, given
/// c = |origin|^2 - radius^2. Callers at the camera compute c from its altitude h above a sphere of radius R as
/// h * (2R + h), which does not cancel at planetary radii. t0 <= t1; false if the ray's line misses the sphere.
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

/// Distance along a ray from `origin` (relative to the sphere's center) to a sphere of `radius`: the nearest positive
/// hit, or -1.
float ray_sphere(float3 origin, float3 direction, float radius) {
    float t0;
    float t1;
    if (!sphere_span(origin, direction, dot(origin, origin) - radius * radius, t0, t1)) {
        return -1.0;
    }
    return t0 >= 0.0 ? t0 : (t1 >= 0.0 ? t1 : -1.0);
}

/// ray_sphere from a point at distance r from the center (r = ground radius + altitude, so c = (r - radius) *
/// (r + radius) keeps the altitude's precision where |origin|^2 - radius^2 would cancel).
float ray_sphere_at(float3 origin, float3 direction, float r, float radius) {
    float t0;
    float t1;
    if (!sphere_span(origin, direction, (r - radius) * (r + radius), t0, t1)) {
        return -1.0;
    }
    return t0 >= 0.0 ? t0 : (t1 >= 0.0 ? t1 : -1.0);
}

/// The shadow test: true when the ray from `p` toward `l` (unit) passes through the sphere of `radius` about the
/// origin of coordinates, i.e. heads inward and passes closer than the radius. A point rounded under the surface does
/// not shadow itself toward a sun above its horizon.
bool sphere_blocks(float3 p, float3 l, float radius) {
    const float b = dot(p, l);
    return b < 0.0 && dot(p, p) - b * b < radius * radius;
}

/// sphere_blocks along a ray: where origin + t * direction (a unit vector) is in the shadow a sphere about the origin
/// of coordinates casts away from the unit direction `l`, the cylinder of its radius behind its center.
/// c = |origin|^2 - radius^2, from the altitude as for sphere_span. Returns (enter, leave), enter >= leave when
/// the ray never is. A march lights the part of each step outside it (lit_fraction), so the shadow's edge moves
/// smoothly across the steps rather than switching a whole step at a time (arcs in twilight, ADR 0094).
float2 sphere_shadow_span(float3 origin, float3 direction, float c, float3 l) {
    const float ol = dot(origin, l);
    const float dl = dot(direction, l);
    // Inside the cylinder: |p - (p.l) l|^2 < radius^2, i.e. a t^2 + 2 b t + cc < 0.
    const float a = max(1.0 - dl * dl, 0.0);
    const float b = dot(origin, direction) - ol * dl;
    const float cc = c - ol * ol;
    float enter = -1e30;
    float leave = 1e30;
    if (a < 1e-6) {  // along the axis: in the cylinder everywhere or nowhere
        if (cc >= 0.0) {
            return float2(1.0, 0.0);
        }
    } else {
        const float discriminant = b * b - a * cc;
        if (discriminant <= 0.0) {
            return float2(1.0, 0.0);
        }
        const float q = b >= 0.0 ? -b - sqrt(discriminant) : -b + sqrt(discriminant);
        const float t0 = q / a;
        const float t1 = q != 0.0 ? cc / q : t0;
        enter = min(t0, t1);
        leave = max(t0, t1);
    }
    // ... and behind the center as seen from the sun: (origin + t * direction).l < 0.
    if (dl > 0.0) {
        leave = min(leave, -ol / dl);
    } else if (dl < 0.0) {
        enter = max(enter, -ol / dl);
    } else if (ol >= 0.0) {
        return float2(1.0, 0.0);
    }
    return float2(enter, leave);
}

/// The fraction of the step [t0, t1] outside `shadow` (sphere_shadow_span), and in `middle` the middle of its longer
/// lit part: where the sun's light is sampled.
float lit_fraction(float2 shadow, float t0, float t1, out float middle) {
    const float a = max(shadow.x, t0);
    const float b = min(shadow.y, t1);
    middle = 0.5 * (t0 + t1);
    if (b <= a) {
        return 1.0;
    }
    const float before = a - t0;
    const float after = t1 - b;
    middle = before >= after ? 0.5 * (t0 + a) : 0.5 * (b + t1);
    return (before + after) / max(t1 - t0, 1e-12);
}

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

/// The sky-view LUT at `uv`: azimuth (x) wraps, elevation (y) stops at the first and last texel centers.
float3 sample_sky_view(SkyFrame s, float2 uv) {
    const float v = clamp(uv.y, 0.5 / sky_view_size.y, 1.0 - 0.5 / sky_view_size.y);
    return s.sky_view.SampleLevel(s.linear_repeat, float2(uv.x, v), 0.0).rgb;
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
    float3 l = sample_sky_view(s, sky_view_uv(s, direction));
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

/// How much of the sky's light reaches air or a surface `distance_km` from the camera (ADR 0082 rule 5): the sky
/// visibility of the camera's surroundings within their reach, rising to 1 over the next reach beyond it.
float sky_visibility_at(SkyFrame s, float distance_km) {
    const float reach = s.occlusion.y;
    if (reach <= 0.0) {
        return 1.0;
    }
    return lerp(s.occlusion.x, 1.0, smoothstep(reach, 2.0 * reach, distance_km));
}

/// The real-time renderer's ambient radiance over the upper hemisphere (nits) on a surface `distance_m` from the
/// camera: the sky's cosine-weighted average, occluded near the camera as the sky visibility says.
float3 sky_ambient_radiance(SkyFrame s, float distance_m) {
    if (s.mode == sky_physical) {
        return s.irradiance.Load(int3(0, 0, 0)).rgb * s.sun.w * sky_visibility_at(s, distance_m * 0.001);
    }
    return s.mode == sky_constant ? s.constant.xyz : float3(0.0, 0.0, 0.0);
}

// ---- the aerial-perspective volume (ADR 0076 decision 3, ADR 0091) ----------------------------------------------
//
// A camera froxel volume: x and y are screen cells, z is how far through the air along each cell's view ray. The
// depth range follows the body, per ray: a ray's slices span the air it crosses, from where it enters the atmosphere
// (the camera, inside it) to where it meets the analytic ground (plus a margin for terrain below it). A ray that misses
// the ground ends at its lowest point from outside the atmosphere, so terrain standing above the ground radius at the
// limb lands at the same dense end as the ground next to it; from inside, its end slides from that lowest point at the
// ground's tangent to the shell's exit at the horizontal, so neighboring rays never jump. So the volume covers hundreds
// of km from orbit and the whole atmosphere at small scale with the same 64 slices, and none is spent on empty space.
// Slices crowd at both ends (aerial_x): near the camera for haze on nearby terrain and the inside of a cave, near the
// ground for the veil seen from orbit. The last slice holds the whole path through the shell, for what lies beyond (a moon seen
// through the limb). A surface is read at its own fraction of its own ray's span, so cells agree on "how far toward
// the ground" even where a grazing view makes neighboring rays meet the ground kilometers apart.

/// A ray's span through the air: where it enters (t_in, km from the camera), how long its slices span, and how long
/// the whole path through the shell is (the last slice's).
struct AerialRay {
    bool valid;  ///< false: the ray misses the atmosphere
    float t_in;
    float span;
    float full;
};

/// The volume's ray parameters for a view direction (world, unit), from the SkyFrame's planet block (filled for the
/// Sky too).
AerialRay aerial_ray(SkyFrame s, float3 direction) {
    AerialRay ray;
    ray.valid = false;
    ray.t_in = 0.0;
    ray.span = 0.0;
    ray.full = 0.0;
    const float bottom = s.atmosphere.planet.x;
    const float top = s.atmosphere.planet.y;
    const float altitude = max(s.origin.w, 0.001);
    const float r = bottom + altitude;
    float t0;
    float t1;
    if (!sphere_span(s.origin.xyz, direction, (r - top) * (r + top), t0, t1) || t1 <= 0.0) {
        return ray;
    }
    float g0;
    float g1;
    const bool ground = sphere_span(s.origin.xyz, direction, altitude * (r + bottom), g0, g1) && g0 > 0.0;
    ray.t_in = max(t0, 0.0);
    const float b = dot(s.origin.xyz, direction);
    const float lowest = clamp(-b, ray.t_in, t1);  // the ray's closest approach to the center, inside the shell
    float t_out = t1;
    if (ground) {
        t_out = g0;
    } else if (t0 > 0.0) {
        t_out = lowest;  // from outside: the lowest point
    } else if (b < 0.0) {
        // From inside, below the horizontal: from the lowest point (at the ground's tangent, where it meets g0) to the
        // exit (at the horizontal), by how high the lowest point is above the ground relative to the camera.
        const float miss = (altitude * (r + bottom) - b * b) / (sqrt(max(r * r - b * b, 0.0)) + bottom);
        t_out = lerp(lowest, t1, smoothstep(0.0, 1.0, saturate(miss / altitude)));
    }
    // The margin: terrain below the ground radius (basins, holes).
    ray.span = max(t_out - ray.t_in, 0.0) * 1.02 + 0.02 * (top - bottom);
    ray.full = ground ? ray.span : max(ray.span, t1 - ray.t_in);  // never through the planet
    ray.valid = true;
    return ray;
}

/// The slice coordinate (0..1) of a fraction x of a ray's span, and its inverse: x = ((1 - cos(pi w)) / 2)^2, so slices
/// go as w^4 near the camera (centimeters to meters first, for a wall in a cave) and crowd at the ground's end as well.
/// render::aerial_slice_w and aerial_slice_x are the twins.
float aerial_w(float x) {
    return acos(clamp(1.0 - 2.0 * sqrt(saturate(x)), -1.0, 1.0)) / atmosphere_pi;
}

float aerial_x(float w) {
    const float f = 0.5 - 0.5 * cos(atmosphere_pi * w);
    return f * f;
}

/// The view direction (world, unit) through screen position `uv` (0..1, y down): the volume's cell rays.
float3 aerial_cell_direction(SkyFrame s, float2 uv) {
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 near = mul(s.inverse_view_projection, float4(ndc, 1.0, 1.0));  // reverse-Z
    const float4 mid = mul(s.inverse_view_projection, float4(ndc, 0.5, 1.0));
    return normalize(mid.xyz / mid.w - near.xyz / near.w);
}

/// Light added (nits) and transmittance (in .a) between the camera and a surface `distance_m` meters along `direction`
/// (world, unit), seen at screen position `uv` (0..1, y down). Identity when the sky is not physical or aerial
/// perspective is off. Both renderers call it on every surface.
float4 aerial_perspective(SkyFrame s, float3 direction, float2 uv, float distance_m) {
    if (s.mode != sky_physical || s.aerial_on == 0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const AerialRay ray = aerial_ray(s, direction);
    const float into_air = distance_m * 0.001 - ray.t_in;
    if (!ray.valid || into_air <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);  // nearer than the atmosphere (a craft in orbit)
    }
    const float slices = float(s.aerial_slices);
    // Past the span: the last slice (the whole path).
    const float w = into_air >= ray.span ? 1.0 : min(aerial_w(into_air / ray.span), 1.0 - 0.5 / slices);
    float4 v = s.aerial.SampleLevel(s.linear_clamp, float3(uv, w), 0.0);
    const float first = 0.5 / slices;
    if (w < first) {  // nearer than the first slice's center: fade toward no effect where the air begins
        v = lerp(float4(0.0, 0.0, 0.0, 1.0), v, w / first);
    }
    return float4(v.rgb * s.sun.w, v.a);
}

// ---- a planet around any center, from the ground to orbit (ADR 0063, 0076) ---------------------------------------
//
// Positions are relative to the planet's center, in km; local up is the direction from the center. All suns are
// summed into one sky-view LUT parameterized by absolute azimuth around local up (from frame_x). The LUT path and the
// per-pixel march call the same integrator (planet_march) with the same step count, so switching between them at the
// top of the atmosphere differs only by the LUT's resolution. Luminance is per lux of `sun.w` (the brightest sun's
// unattenuated illuminance); each sun's own illuminance is sun_colors[i] in those units.

float3 planet_up(SkyFrame s) {
    return normalize(s.origin.xyz);
}

/// The other horizontal axis of the sky-view LUT's frame (frame_x, up, frame_z is right-handed).
float3 planet_frame_z(SkyFrame s) {
    return cross(s.frame_x.xyz, planet_up(s));
}

/// The visible fraction of one sun's disk (angular radius `sun_radius`) toward `l` seen from `p`, past one occluder
/// sphere: the area of the sun's disk outside the occluder's, as flat disks (the lens-area formula;
/// space::disk_visible_fraction is the CPU twin, checked against a numeric integral).
float disk_visible(float3 p, float3 l, float sun_radius, float4 occluder) {
    const float3 d = occluder.xyz - p;
    const float along = dot(d, l);
    if (along <= 0.0) {
        return 1.0;  // the occluder is behind
    }
    const float distance = length(d);
    if (distance <= occluder.w) {
        return 0.0;
    }
    // Most samples are far from any eclipse: reject without trigonometry when the separation exceeds the radii's
    // sum, cos(separation) < cos(a + b) (b is small: cos b and sin b to second order).
    const float sin_a = occluder.w / distance;
    const float cos_a = sqrt(1.0 - sin_a * sin_a);
    const float b = sun_radius;
    if (along / distance < cos_a * (1.0 - 0.5 * b * b) - sin_a * b) {
        return 1.0;
    }
    const float a = asin(sin_a);                                         // the occluder's angular radius
    const float c = 2.0 * asin(min(length(d / distance - l) * 0.5, 1.0));  // separation, precise at small angles
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

/// The visible fraction of sun `i`'s disk from `p`, past every occluder (eclipses per sample, ADR 0076 decision 2).
/// Overlapping occluders multiply, which is exact when at most one covers the disk.
float planet_sun_visible(SkyFrame s, float3 p, uint i) {
    float visible = 1.0;
    for (uint k = 0; k < s.occluder_count; ++k) {
        visible *= disk_visible(p, s.sun_directions[i].xyz, s.sun_directions[i].w, s.occluders[k]);
    }
    return visible;
}

/// Transmittance of sun `i`'s light to `p` (0 in the planet's shadow); `visible` is the fraction of its disk the
/// occluders leave.
float3 planet_sun_transmittance(SkyFrame s, float3 p, uint i, out float visible) {
    const float3 l = s.sun_directions[i].xyz;
    visible = planet_sun_visible(s, p, i);
    if (sphere_blocks(p, l, s.atmosphere.planet.x)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float r = length(p);
    return sample_transmittance(s, r, dot(p / r, l));
}

/// Hillaire's multiple-scattering term at `p` for sun `i` (per lux of that sun).
float3 planet_multiple_scattering(SkyFrame s, float3 p, uint i) {
    const float r = length(p);
    const float mu = dot(p / r, s.sun_directions[i].xyz);
    const float height = (r - s.atmosphere.planet.x) / (s.atmosphere.planet.y - s.atmosphere.planet.x);
    return s.multiscattering.SampleLevel(s.linear_clamp, float2(mu * 0.5 + 0.5, saturate(height)), 0.0).rgb;
}

/// Per-sun phase values along one view direction.
struct SunPhases {
    float rayleigh[planet_max_suns];
    float mie[planet_max_suns];
};

SunPhases sun_phases(SkyFrame s, float3 direction) {
    SunPhases ph;
    for (uint i = 0; i < planet_max_suns; ++i) {
        const float cos_theta = dot(direction, s.sun_directions[i].xyz);
        ph.rayleigh[i] = rayleigh_phase(cos_theta);
        ph.mie[i] = mie_phase(cos_theta, s.atmosphere.mie_absorption.w);
    }
    return ph;
}

/// Where a camera ray (from `origin`, `altitude` above the ground) is in the planet's shadow, for each sun.
struct PlanetShadows {
    float4 enter;  ///< per sun, km along the ray (sphere_shadow_span)
    float4 leave;
};

PlanetShadows planet_shadows(SkyFrame s, float3 origin, float altitude, float3 direction) {
    PlanetShadows sh;
    const float bottom = s.atmosphere.planet.x;
    const float c = altitude * (2.0 * bottom + altitude);
    sh.enter = float4(1.0, 1.0, 1.0, 1.0);
    sh.leave = float4(0.0, 0.0, 0.0, 0.0);
    for (uint i = 0; i < s.sun_count; ++i) {
        const float2 span = sphere_shadow_span(origin, direction, c, s.sun_directions[i].xyz);
        sh.enter[i] = span.x;
        sh.leave[i] = span.y;
    }
    return sh;
}

/// Light scattered toward the viewer per km (per lux of sun.w) over the step [t0, t1] of the ray origin + t *
/// direction, every sun with its eclipses; `m` is the medium at the step's middle. Each sun lights the part of the
/// step outside the planet's shadow (`sh`), its light sampled in that part.
float3 planet_in_scatter(SkyFrame s, float3 origin, float3 direction, float t0, float t1, Medium m, SunPhases ph,
                         PlanetShadows sh) {
    const float3 p = origin + direction * (0.5 * (t0 + t1));
    float3 in_scatter = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < s.sun_count; ++i) {
        const float3 l = s.sun_directions[i].xyz;
        float middle;
        const float lit = lit_fraction(float2(sh.enter[i], sh.leave[i]), t0, t1, middle);
        const float3 q = origin + direction * middle;  // p unless the step's middle is in the shadow
        float3 single = float3(0.0, 0.0, 0.0);
        if (lit > 0.0) {
            const float r = length(q);
            single = (m.rayleigh * ph.rayleigh[i] + m.mie * ph.mie[i]) * sample_transmittance(s, r, dot(q / r, l)) * lit;
        }
        in_scatter += (single + m.scattering * planet_multiple_scattering(s, p, i)) * s.sun_colors[i].xyz *
                      planet_sun_visible(s, q, i);
    }
    return in_scatter;
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
    const SunPhases ph = sun_phases(s, direction);
    const PlanetShadows sh = planet_shadows(s, origin, altitude, direction);
    const float dt = (end - start) / float(steps);
    for (uint k = 0; k < steps; ++k) {
        const float3 p = origin + direction * (start + (float(k) + 0.5) * dt);
        const Medium m = medium_at(s.atmosphere, length(p) - bottom);
        const float3 step_transmittance = exp(-m.extinction * dt);
        const float3 in_scatter =
            planet_in_scatter(s, origin, direction, start + float(k) * dt, start + float(k + 1) * dt, m, ph, sh);
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

// ---- spike SP-14 (VCH-090): a procedural starfield ---------------------------------------------------------------
//
// One star or none per cell of a cube map with stars.w cells per face side, so the field is fixed on the sky. Counts
// follow the real sky (N(< m) = 4800 * 10^(0.49 (m - 6)): about 15 stars brighter than 1, 4800 than 6) up to the
// limiting magnitude. Each draws as an energy-conserving point: a Gaussian of 0.6 px whose integral over solid angle is
// its illuminance, E = 2.5e-6 lux * 10^(-0.4 m) (ADR 0063), so exposure sees stars physically. Stands in for the galaxy
// query's near stars.

uint star_hash(uint3 v) {  // pcg3d (Jarzynski and Olano 2020)
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v.x ^ v.y ^ v.z;
}

float star_unit(uint h) {
    return (float(h >> 8) + 0.5) / 16777216.0;
}

float3 star_face_direction(uint face, float2 uv) {
    switch (face) {
    case 0: return float3(1.0, uv.y, -uv.x);
    case 1: return float3(-1.0, uv.y, uv.x);
    case 2: return float3(uv.x, 1.0, -uv.y);
    case 3: return float3(uv.x, -1.0, uv.y);
    case 4: return float3(uv.x, uv.y, 1.0);
    default: return float3(-uv.x, uv.y, -1.0);
    }
}

/// The starfield's radiance toward `d` (nits).
float3 starfield_radiance(SkyFrame s, float3 d) {
    if (s.stars.x <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 a = abs(d);
    uint face;
    float2 uv;
    if (a.x >= a.y && a.x >= a.z) {
        face = d.x > 0.0 ? 0u : 1u;
        uv = float2(d.x > 0.0 ? -d.z : d.z, d.y) / a.x;
    } else if (a.y >= a.z) {
        face = d.y > 0.0 ? 2u : 3u;
        uv = float2(d.x, d.y > 0.0 ? -d.z : d.z) / a.y;
    } else {
        face = d.z > 0.0 ? 4u : 5u;
        uv = float2(d.z > 0.0 ? d.x : -d.x, d.y) / a.z;
    }
    const float n = s.stars.w;
    const int2 cell = int2(floor((uv * 0.5 + 0.5) * n));
    const float total = 4800.0 * pow(10.0, 0.49 * (s.stars.z - 6.0));  // stars brighter than the limit
    const float p_mean = total / (6.0 * n * n);  // the mean chance a cell has one
    const float sigma = 0.6 * s.stars.y;
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 c = cell + int2(dx, dy);
            if (any(c < 0) || any(c >= int(n))) {
                continue;
            }
            // Cells near a face's corners cover less sky (solid angle ~ 1 / (1 + u^2 + v^2)^1.5; 4 pi / 6 on
            // average, hence 6 / pi): the chance follows it, so the density is the same everywhere.
            const float2 st = (float2(c) + 0.5) / n * 2.0 - 1.0;
            const float p = min(p_mean * (6.0 / atmosphere_pi) / pow(1.0 + dot(st, st), 1.5), 1.0);
            const uint h = star_hash(uint3(uint(c.x), uint(c.y), face));
            const float u = star_unit(h);
            if (u > p) {
                continue;
            }
            const uint h2 = star_hash(uint3(h, 0x5a1u, face));
            const float2 jitter = float2(star_unit(h2), star_unit(h2 * 747796405u + 2891336453u));
            const float3 direction = normalize(star_face_direction(face, (float2(c) + jitter) / n * 2.0 - 1.0));
            const float magnitude = 6.0 + log10(max(u / p, 1e-9) * total / 4800.0) / 0.49;
            const float illuminance = 2.5e-6 * pow(10.0, -0.4 * magnitude);
            const float chord = length(d - direction);  // the angle, for the few pixels that matter
            const float psf = exp(-chord * chord / (2.0 * sigma * sigma)) / (2.0 * atmosphere_pi * sigma * sigma);
            // From blue-white to orange, normalized to unit luminance.
            const float3 tint = lerp(float3(0.78, 0.88, 1.18), float3(1.22, 0.96, 0.70), star_unit(h2 * 2654435761u));
            sum += tint / dot(tint, float3(0.2126, 0.7152, 0.0722)) * (illuminance * psf);
        }
    }
    return sum * s.stars.x;
}

/// True when this view uses the per-pixel march rather than the sky-view LUT.
bool planet_marches(SkyFrame s) {
    return s.march == 1 || (s.march == 0 && s.atmosphere.planet.x + s.origin.w >= s.atmosphere.planet.y);
}

/// The sky's radiance toward `direction` in planet mode (nits): the atmosphere (LUT or march) with the ground, the
/// occluders the sky draws (spheres lit by the suns, beyond the atmosphere) and, with `with_sun`, the suns' disks.
float3 planet_sky_radiance(SkyFrame s, float3 direction, bool with_sun) {
    const float3 origin = s.origin.xyz;
    const float bottom = s.atmosphere.planet.x;
    const float altitude = max(s.origin.w, 0.0);
    float3 l;
    float3 t;
    if (planet_marches(s)) {
        planet_march(s, origin, altitude, direction, 1e9, s.march_steps, true, l, t);
    } else {
        l = sample_sky_view(s, planet_sky_view_uv(s, direction));
        float g0;
        float g1;
        const bool ground =
            sphere_span(origin, direction, altitude * (2.0 * bottom + altitude), g0, g1) && g0 > 0.0;
        t = ground ? float3(0.0, 0.0, 0.0) :
                     sample_transmittance(s, bottom + altitude, dot(direction, planet_up(s)));
    }
    if (any(t > 0.0)) {
        // The nearest occluder the sky draws, if the ray meets one (they are beyond the atmosphere: the in-scattering
        // in front is `l`).
        float nearest = 1e30;
        uint hit_index = planet_max_occluders;
        for (uint k = 0; k < s.occluder_count; ++k) {
            if (s.occluder_albedo[k].w <= 0.0) {
                continue;
            }
            const float3 to_center = s.occluders[k].xyz - origin;
            const float tc = dot(to_center, direction);
            const float miss2 = dot(to_center, to_center) - tc * tc;
            const float r2 = s.occluders[k].w * s.occluders[k].w;
            if (tc > 0.0 && miss2 < r2 && tc - sqrt(r2 - miss2) < nearest) {
                nearest = tc - sqrt(r2 - miss2);
                hit_index = k;
            }
        }
        if (hit_index < planet_max_occluders) {
            const float3 n = normalize(origin + direction * nearest - s.occluders[hit_index].xyz);
            float3 irradiance = float3(0.0, 0.0, 0.0);
            for (uint i = 0; i < s.sun_count; ++i) {
                irradiance += s.sun_colors[i].xyz * saturate(dot(n, s.sun_directions[i].xyz));
            }
            l += t * irradiance * s.occluder_albedo[hit_index].xyz / atmosphere_pi;
        } else if (with_sun) {
            for (uint i = 0; i < s.sun_count; ++i) {
                const float radius = s.sun_directions[i].w;
                if (dot(direction, s.sun_directions[i].xyz) > cos(radius)) {
                    l += t * s.sun_colors[i].xyz / (2.0 * atmosphere_pi * (1.0 - cos(radius)));
                }
            }
            if (s.stars.x > 0.0) {
                l += t * starfield_radiance(s, direction) / s.sun.w;
            }
        }
    }
    return l * s.sun.w;
}

/// sky_radiance for the real-time renderer: the planet's sky in planet mode, the Sky's otherwise. Photo mode keeps
/// calling sky_radiance (the path tracer's kernel size is near Metal's compiler limit).
float3 sky_radiance_any(SkyFrame s, float3 direction, bool with_sun) {
    if (s.planet != 0 && s.mode == sky_physical) {
        return planet_sky_radiance(s, direction, with_sun);
    }
    if (s.mode == sky_none && with_sun && s.stars.x > 0.0) {  // deep space
        return starfield_radiance(s, direction);
    }
    return sky_radiance(s, direction, with_sun);
}

/// The transmittance from the camera toward `direction` to what lies beyond the sky (the space backdrop, ADR 0091):
/// 1 with no sky, 0 for a constant sky or through the ground or the occluder, the transmittance LUT's through the air
/// (from the camera, or from where the ray enters the atmosphere when the camera is above it).
float3 sky_to_space(SkyFrame s, float3 direction) {
    if (s.mode == sky_none) {
        return float3(1.0, 1.0, 1.0);
    }
    if (s.mode != sky_physical) {
        return float3(0.0, 0.0, 0.0);
    }
    if (s.planet == 0) {
        const float r = s.atmosphere.planet.x + s.camera.x;
        return ray_sphere(float3(0.0, r, 0.0), direction, s.atmosphere.planet.x) < 0.0 ?
                   sample_transmittance(s, r, direction.y) :
                   float3(0.0, 0.0, 0.0);
    }
    const float3 origin = s.origin.xyz;
    const float bottom = s.atmosphere.planet.x;
    const float top = s.atmosphere.planet.y;
    if (ray_sphere(origin, direction, bottom) >= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    for (uint k = 0; k < s.occluder_count; ++k) {
        const float3 to_occluder = s.occluders[k].xyz - origin;
        const float tc = dot(to_occluder, direction);
        if (tc > 0.0 && dot(to_occluder, to_occluder) - tc * tc < s.occluders[k].w * s.occluders[k].w) {
            return float3(0.0, 0.0, 0.0);
        }
    }
    const float r = length(origin);
    if (r <= top) {
        return sample_transmittance(s, max(r, bottom), dot(direction, origin / max(r, 1e-6)));
    }
    const float entry = ray_sphere(origin, direction, top);
    if (entry < 0.0) {
        return float3(1.0, 1.0, 1.0);
    }
    const float3 p = origin + direction * entry;
    return sample_transmittance(s, top, dot(direction, p / top));
}
