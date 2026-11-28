// The procedural space backdrop (docs/specs/space-backdrop/spec.md, ADR 0091): stars per pixel, and the baked band
// and nebulae (shaders/backdrop.slang) sampled from a lat-long texture in galactic coordinates. Everything is in the
// backdrop's galactic frame: +X toward the galactic center, +Y the north galactic pole. SkyFrame (veach.atmosphere)
// carries the rotation from render space, the camera's axes and the creator's exposure (render::SpaceBackdrop).

import veach.atmosphere;
import veach.bindless;

static const float backdrop_pi = 3.14159265358979;

// ---- hashing and noise -----------------------------------------------------------------------------------------

public uint3 backdrop_pcg3d(uint3 v) {  // Jarzynski and Olano 2020
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

public uint backdrop_hash(uint3 v) {
    const uint3 h = backdrop_pcg3d(v);
    return h.x ^ h.y ^ h.z;
}

/// Uniform in (0, 1).
public float backdrop_unit(uint h) {
    return (float(h >> 8) + 0.5) / 16777216.0;
}

/// Gradient noise on the integer lattice offset by `seed`, about -1..1.
public float backdrop_noise(float3 p, uint seed) {
    const float3 cell = floor(p);
    const float3 f = p - cell;
    const int3 i = int3(cell);
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float corners[8];
    for (uint k = 0; k < 8; ++k) {
        const int3 o = int3(int(k & 1u), int((k >> 1) & 1u), int((k >> 2) & 1u));
        const uint3 h = backdrop_pcg3d(uint3(i + o) + uint3(seed, seed * 747796405u, seed * 2891336453u));
        const float3 g = float3(h & 0xFFFFu) / 32767.5 - 1.0;
        corners[k] = dot(g, f - float3(o));
    }
    const float x0 = lerp(lerp(corners[0], corners[1], u.x), lerp(corners[2], corners[3], u.x), u.y);
    const float x1 = lerp(lerp(corners[4], corners[5], u.x), lerp(corners[6], corners[7], u.x), u.y);
    return lerp(x0, x1, u.z) * 1.6;
}

/// Fractal noise: `octaves` of backdrop_noise, each turned (against lattice alignment) and 2.03 times finer.
public float backdrop_fbm(float3 p, uint seed, uint octaves) {
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (uint o = 0; o < octaves; ++o) {
        sum += amplitude * backdrop_noise(p, seed + o * 101u);
        total += amplitude;
        p = float3(0.80 * p.x - 0.60 * p.z, p.y * 0.96 + p.x * 0.28, 0.60 * p.x + 0.80 * p.z) * 2.03 + 17.1;
        amplitude *= 0.5;
    }
    return sum / total;
}

/// Domain-warped fractal noise (Quilez): the point moved twice by noise of itself. `warp` is the vector noise of the
/// first step, for shapes that follow the same flow.
public float backdrop_warped(float3 p, uint seed, uint octaves, float strength, out float3 warp) {
    warp = float3(backdrop_fbm(p, seed, 4), backdrop_fbm(p + 5.2, seed + 7u, 4), backdrop_fbm(p + 9.7, seed + 13u, 4));
    const float3 q = p + strength * warp;
    const float3 r = float3(backdrop_fbm(q + 1.7, seed + 19u, 4), backdrop_fbm(q + 8.3, seed + 23u, 4),
                            backdrop_fbm(q + 2.8, seed + 29u, 4));
    return backdrop_fbm(p + strength * r, seed + 31u, octaves);
}

// ---- the baked texture -----------------------------------------------------------------------------------------

/// The lat-long texture's coordinates of a galactic direction: u = longitude (0.5 toward the center), v = colatitude.
public float2 backdrop_uv(float3 g) {
    return float2(atan2(g.z, g.x) / (2.0 * backdrop_pi) + 0.5, acos(clamp(g.y, -1.0, 1.0)) / backdrop_pi);
}

/// The galactic direction of a texel's coordinates (backdrop_uv's inverse).
public float3 backdrop_direction(float2 uv) {
    const float longitude = (uv.x - 0.5) * 2.0 * backdrop_pi;
    const float colatitude = uv.y * backdrop_pi;
    const float s = sin(colatitude);
    return float3(s * cos(longitude), cos(colatitude), s * sin(longitude));
}

// ---- stars -----------------------------------------------------------------------------------------------------

float3 backdrop_face_direction(uint face, float2 uv) {
    switch (face) {
    case 0: return float3(1.0, uv.y, -uv.x);
    case 1: return float3(-1.0, uv.y, uv.x);
    case 2: return float3(uv.x, 1.0, -uv.y);
    case 3: return float3(uv.x, -1.0, uv.y);
    case 4: return float3(uv.x, uv.y, 1.0);
    default: return float3(-uv.x, uv.y, -1.0);
    }
}

void backdrop_face_of(float3 d, out uint face, out float2 uv) {
    const float3 a = abs(d);
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
}

/// Krystek's 1985 fit of the Planckian locus (CIE 1960 uv) to linear Rec. 709 at unit luminance: the same as
/// render::blackbody_color.
public float3 backdrop_blackbody(float kelvin) {
    const float t = clamp(kelvin, 1000.0, 15000.0);
    const float u = (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t) /
                    (1.0 + 8.42420235e-4 * t + 7.08145163e-7 * t * t);
    const float v = (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t) /
                    (1.0 - 2.89741816e-5 * t + 1.61456053e-7 * t * t);
    const float d = 2.0 * u - 8.0 * v + 4.0;
    const float x = 3.0 * u / d;
    const float y = 2.0 * v / d;
    const float3 xyz = float3(x / y, 1.0, (1.0 - x - y) / y);
    const float3 rgb = float3(3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z,
                              -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z,
                              0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z);
    return max(rgb, float3(0.0, 0.0, 0.0));
}

static const float backdrop_luma_r = 0.2126;
static const float backdrop_luma_g = 0.7152;
static const float backdrop_luma_b = 0.0722;

float backdrop_luminance(float3 c) {
    return dot(c, float3(backdrop_luma_r, backdrop_luma_g, backdrop_luma_b));
}

/// A cheap 32-bit integer hash (Wellons' lowbias32): the stars' per-cell randomness.
uint backdrop_lowbias(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// A star's temperature (K) from a uniform `u` and `v`, `bright` 0 for the faintest stars drawn and 1 for the
/// brightest. Faint stars are mostly K and M dwarfs; bright ones mostly A and B stars and K giants, as in the real sky.
float backdrop_temperature(float u, float v, float bright) {
    // The share of M, K, G, F, A (B: the rest) among the faintest and among the brightest, accumulated.
    const float m = lerp(0.24, 0.07, bright);
    const float k = m + lerp(0.33, 0.27, bright);
    const float g = k + lerp(0.19, 0.10, bright);
    const float f = g + lerp(0.12, 0.12, bright);
    const float a = f + lerp(0.08, 0.24, bright);
    float2 range = float2(10000.0, 28000.0);  // B
    if (u < m) {
        range = float2(2600.0, 3700.0);
    } else if (u < k) {
        range = float2(3700.0, 5200.0);
    } else if (u < g) {
        range = float2(5200.0, 6000.0);
    } else if (u < f) {
        range = float2(6000.0, 7500.0);
    } else if (u < a) {
        range = float2(7500.0, 10000.0);
    }
    return range.x * exp2(v * log2(range.y / range.x));
}

/// A star's tint: its black-body color pushed from white by `saturation`, at unit luminance.
float3 backdrop_tint(float kelvin, float saturation) {
    const float3 c = backdrop_blackbody(kelvin);
    const float3 pushed = max(lerp(float3(1.0, 1.0, 1.0), c / max(backdrop_luminance(c), 1e-4), saturation),
                              float3(0.0, 0.0, 0.0));
    return pushed / max(backdrop_luminance(pushed), 1e-4);
}

static const float backdrop_field_cells = 448.0;  ///< per cube face side: at most one faint star each
static const float backdrop_bright_cells = 14.0;  ///< per face side: at most one bright star each
static const float backdrop_field_p = 0.14;       ///< a field cell's chance of a star, before density
static const float backdrop_bright_p = 0.17;      ///< a bright cell's chance: about 200 bright stars
static const float backdrop_field_limit = 10.0;   ///< the faintest magnitude drawn
static const float backdrop_split = 2.5;          ///< field stars are fainter, bright stars brighter
static const float backdrop_brightest = -1.5;
static const float backdrop_count_slope = 0.45;  ///< log10 N(< m) grows by this per magnitude

/// The direction of a point of a cube face cell grid: cell `c` (n per side), at `jitter` inside it.
float3 backdrop_cell_point(uint face, int2 c, float2 jitter, float n) {
    return normalize(backdrop_face_direction(face, (float2(c) + jitter) / n * 2.0 - 1.0));
}

/// The stars' radiance toward galactic direction `g` (nits). `density` is the baked star density there (the band's
/// star clouds, less behind dust). `pixel` is one pixel's angle; `right` and `up` the camera's axes (galactic).
/// Illuminance follows magnitude (ADR 0063: 2.5e-6 lux at m = 0, magnitudes scaled by `magnitude_scale`); counts
/// follow log10 N(< m) = 0.45 m.
public float3 backdrop_stars(float3 g, float density, float pixel, float3 right, float3 up, uint seed,
                             float star_density, float magnitude_scale, float saturation) {
    uint face;
    float2 uv;
    backdrop_face_of(g, face, uv);
    float3 sum = float3(0.0, 0.0, 0.0);

    // The field: hundreds of thousands of faint stars, more where the band is, as energy-conserving points. A star
    // lights pixels within 3 sigma (2 pixels at 1440p, less than half a cell), so only the pixel's cell and its three
    // nearest neighbors can hold one.
    {
        const float n = backdrop_field_cells;
        const float sigma = 0.65 * pixel;
        const float2 at = (uv * 0.5 + 0.5) * n;
        const int2 cell = int2(floor(at));
        const int2 toward = int2(frac(at.x) < 0.5 ? -1 : 1, frac(at.y) < 0.5 ? -1 : 1);
        const float p = min(backdrop_field_p * star_density * density, 1.0);
        const uint base = backdrop_lowbias(seed ^ (face * 0x9e3779b9u));
        for (uint k = 0; k < 4; ++k) {
            const int2 c = cell + int2((k & 1u) != 0 ? toward.x : 0, (k & 2u) != 0 ? toward.y : 0);
            if (any(c < 0) || any(c >= int(n))) {
                continue;
            }
            const uint h = backdrop_lowbias(base ^ (uint(c.x) + uint(c.y) * 1031u));
            const float u = float(h) * (1.0 / 4294967296.0);
            if (u >= p) {
                continue;
            }
            const uint h2 = backdrop_lowbias(h);
            const float2 jitter = float2(float(h2 & 0xFFFFu), float(h2 >> 16)) * (1.0 / 65536.0);
            const float3 offset = g - backdrop_cell_point(face, c, jitter, n);
            const float d2 = dot(offset, offset);
            if (d2 > 9.0 * sigma * sigma) {
                continue;
            }
            // u / p is uniform: the rank among stars, brightest at 0.
            const float m = backdrop_field_limit + log2(max(u / p, 4.2e-4)) * (0.30103 / backdrop_count_slope);
            const float bright = saturate((backdrop_field_limit - m) * (1.0 / (backdrop_field_limit - backdrop_brightest)));
            const uint h3 = backdrop_lowbias(h2);
            const float kelvin = backdrop_temperature(float(h3 & 0xFFFFu) * (1.0 / 65536.0),
                                                      float(h3 >> 16) * (1.0 / 65536.0), bright);
            const float e = 2.5e-6 * exp2(-1.3287712 * m * magnitude_scale);  // 10^(-0.4 m)
            const float psf = exp(-d2 / (2.0 * sigma * sigma)) / (2.0 * backdrop_pi * sigma * sigma);
            sum += backdrop_tint(kelvin, saturation) * (e * psf);
        }
    }

    // The bright stars: a few hundred over the sky, each with a halo and four diffraction spikes along the camera's
    // axes (a lens's spider vanes), longer for brighter stars. Spikes are sized on the sky (up to 0.016 rad, 22 pixels
    // at 1440p and 60 degrees) and end within half a cell, so the pixel's cell and its three nearest neighbors are
    // enough here too.
    {
        const float n = backdrop_bright_cells;
        const float sigma = 0.75 * pixel;
        const float2 at = (uv * 0.5 + 0.5) * n;
        const int2 cell = int2(floor(at));
        const int2 toward = int2(frac(at.x) < 0.5 ? -1 : 1, frac(at.y) < 0.5 ? -1 : 1);
        const uint base = backdrop_lowbias((seed ^ 0xa511e9b3u) + face * 0x632be5abu);
        for (uint k = 0; k < 4; ++k) {
            const int2 c = cell + int2((k & 1u) != 0 ? toward.x : 0, (k & 2u) != 0 ? toward.y : 0);
            {
                if (any(c < 0) || any(c >= int(n))) {
                    continue;
                }
                const uint h = backdrop_lowbias(base ^ (uint(c.x) + uint(c.y) * 131u));
                const float u = float(h) * (1.0 / 4294967296.0);
                if (u >= backdrop_bright_p) {
                    continue;
                }
                const uint h2 = backdrop_lowbias(h);
                const float2 jitter = float2(float(h2 & 0xFFFFu), float(h2 >> 16)) * (0.8 / 65536.0) + 0.1;
                const float3 offset = g - backdrop_cell_point(face, c, jitter, n);
                const float2 q = float2(dot(offset, right), dot(offset, up)) / pixel;  // pixels, camera axes
                const float m = backdrop_split + log2(max(u / backdrop_bright_p, 0.0158)) * (0.30103 / backdrop_count_slope);
                const float glare = saturate((backdrop_split - m) * (1.0 / (backdrop_split - backdrop_brightest)));
                const float spike_length = (5.0e-4 + 0.0145 * glare * glare) / pixel;  // pixels
                const float reach = max(6.0, spike_length * 3.5);
                if (abs(q.x) > reach || abs(q.y) > reach || dot(offset, offset) > 0.25) {
                    continue;
                }
                const uint h3 = backdrop_lowbias(h2);
                const float kelvin = backdrop_temperature(float(h3 & 0xFFFFu) * (1.0 / 65536.0),
                                                          float(h3 >> 16) * (1.0 / 65536.0), 0.6 + 0.4 * glare);
                const float e = 2.5e-6 * exp2(-1.3287712 * m * magnitude_scale);
                const float d2 = dot(offset, offset);
                float shape = exp(-d2 / (2.0 * sigma * sigma)) / (2.0 * backdrop_pi * sigma * sigma);
                // Per pixel area (1 / pixel^2): a halo of 1 / (1 + r^2 / r0^2)^2 and two spikes, a few percent each.
                const float r0 = 2.0 + 2.0 * glare;
                const float r2 = dot(q, q) / (r0 * r0);
                float extra = 0.05 * glare / (backdrop_pi * r0 * r0) / ((1.0 + r2) * (1.0 + r2));
                const float width = 0.55;
                const float across = 0.03 * glare / (2.0 * spike_length * sqrt(2.0 * backdrop_pi) * width);
                extra += across * exp(-abs(q.x) / spike_length - q.y * q.y / (2.0 * width * width));
                extra += across * exp(-abs(q.y) / spike_length - q.x * q.x / (2.0 * width * width));
                shape += extra / (pixel * pixel);
                sum += backdrop_tint(kelvin, saturation) * (e * shape);
            }
        }
    }
    return sum;
}

// ---- in a frame ------------------------------------------------------------------------------------------------

/// True when the frame has a backdrop to draw.
public bool backdrop_on(SkyFrame s) {
    return s.backdrop_info.w != 0u;
}

/// The galactic direction of render-space direction `d`.
public float3 backdrop_galactic(SkyFrame s, float3 d) {
    return float3(dot(s.galactic_x.xyz, d), dot(s.galactic_y.xyz, d), dot(s.galactic_z.xyz, d));
}

/// The backdrop's radiance toward render-space direction `d` (nits, physical: before the creator's gain).
public float3 backdrop_radiance(SkyFrame s, float3 d) {
    const float3 g = backdrop_galactic(s, d);
    const float2 size = float2(float(s.backdrop_info.y), float(s.backdrop_info.z));
    float2 uv = backdrop_uv(g);
    uv.y = clamp(uv.y, 0.5 / size.y, 1.0 - 0.5 / size.y);
    const float4 baked = s.backdrop.SampleLevel(s.linear_repeat, uv, 0.0);
    const float3 stars = backdrop_stars(g, baked.a, s.galactic_x.w, s.camera_right.xyz, s.camera_up.xyz,
                                        s.backdrop_info.x, s.backdrop_stars.x, s.backdrop_stars.y, s.backdrop_stars.z);
    return baked.rgb + stars;
}

/// The creator's gain on the backdrop for a pixel exposed by `exposure` (render::SpaceBackdrop): the backdrop is
/// shown `exposure_compensation` stops above its physical brightness at EV100 -4 (deep space), and follows the
/// camera's exposure by `exposure_follow` (1: physical; 0: the same at every exposure).
///
/// The sky in front hides the boost, as a lit sky hides the stars (`sky_exposed` is the pixel's exposed sky radiance
/// luminance: the air's light along the ray, the limb's glow; docs/specs/space-backdrop/spec.md, "The sky in front
/// hides the boost"). `v` is that light, physical, over the backdrop's own (its band's brightest, backdrop_stars.w);
/// `x = v / g` is the same in the image, against the boosted backdrop. The boost above physical is divided by
/// (1 + v)^w, w going smoothly (in log x) from 0 where the sky is under half the boosted backdrop in the image to 1
/// where it is twice as bright: only a sky both physically brighter than the backdrop and visible against its boost
/// hides it. Under a black sky (space, the night side, a moonless night) it is whole; under a day sky (v ~ 1e7, x
/// ~ 5) it is gone and the backdrop is physical, millions of times darker than the sky; the air around a planet's
/// limb seen from orbit, physically brighter than the band but dimmer than it in the image, keeps it; in twilight it
/// comes back as the sky darkens, the brightest stars first.
public float backdrop_gain(SkyFrame s, float exposure, float sky_exposed) {
    const float ev100 = log2(1.0 / (1.2 * max(exposure, 1e-30)));
    const float stops = s.galactic_y.w + (1.0 - s.galactic_z.w) * (ev100 + 4.0);
    const float gain = exp2(clamp(stops, -40.0, 40.0));
    const float v = sky_exposed / max(exposure, 1e-30) * s.backdrop_stars.w;
    const float x = v / gain;
    if (x <= 0.5) {
        return gain;  // a black sky, or one dimmer than the boosted backdrop in the image (w = 0)
    }
    const float t = saturate((log2(x) + 1.0) * 0.5);  // x = 0.5: 0, x = 2: 1
    const float w = t * t * (3.0 - 2.0 * t);
    return 1.0 + (gain - 1.0) * exp2(-w * log2(1.0 + v));
}
