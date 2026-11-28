// Large-scale sky occlusion for the real-time renderer (docs/specs/sky-occlusion/spec.md): which parts of the sky the
// scene hides from a shaded point, out to tens of meters, by direction, and how sunlit the surroundings that hide it
// are. GTAO (ambient_occlusion.slang) reaches 1.5 m in screen space; this reaches the buildings across a street, a
// cave's ceiling and the walls of a dug hole, on screen or not.
//
// sky_occlusion.cpp renders a height map of the scene around the camera from above, two layers: the highest surface
// of each texel and the lowest. The lowest surface is the ground, solid below; whatever stands above it is a slab
// under its top (a roof, an eave, a cave's ceiling, a canopy), open below; a building whose shell has no floor is a
// column, its lowest surface being its roof. sky_occlusion.slang's trace walks that map from the point of every
// 8 x 8 pixel cell in 8 fixed azimuths and finds, per azimuth, the arc of the upper sky (0 to 90 degrees of elevation)
// the columns and slabs hide, and whether the air in front of what the point sees there is in the sun (the sun shadow
// maps). It then evaluates the ambient table (veach/ambient.slang) with and without what is hidden, for the pixel's
// normal rebuilt from depth, and stores the ratio: what share of the sky's diffuse and specular ambient light the
// scene leaves the point.
//
// The shading reads the 4 nearest cells at its own distance (sky_occlusion_at) and ambient_light()
// multiplies the sky's light by their ratio. The lower half of the sphere stays the table's ground, scaled by how much
// of what the point sees there is sunlit.

import veach.bindless;

static const uint sky_azimuths = 8;
/// Full-resolution pixels per side of the cells the occlusion is traced in: cell i stands for pixel 8i + 4.
static const uint sky_cell = 8;
static const float sky_pi = 3.14159265358979;
static const float sky_half_pi = 1.5707963267949;
/// sin(pi / 8) / (pi / 8): the mean of cos(phi - phi_k) over an azimuth's sector, relative to its center.
static const float sky_sector_sinc = 0.9744953;

/// Mirrors render::SkyOcclusionFrame (sky_occlusion.hpp): the height map's frame and the trace's results.
struct SkyOcclusionFrame {
    float4 center;  ///< xyz: the height map's center, render space (snapped to its texels); w: its half-width (m)
    float4 up;      ///< xyz: the map's up (the camera's); w: meters per texel of mip 0
    float4 axis_x;  ///< xyz: azimuth 0, perpendicular to up; w: the map's depth range (m)
    float4 axis_y;  ///< xyz: azimuth 90 degrees (axis_x x axis_y = up); w: how far its bottom lies below its center (m)
    float4 reach;   ///< x, y: the occlusion fades out between these distances from the camera (m); z: the map's mips
    float4 lights[2];  ///< xyz: toward each light ShadowFrame shadows, by slot; w: 1 if the slot is used
    /// Per cell: rgb the share of the sky's diffuse ambient light the scene leaves, a the share
    /// of its specular light (rgba16_float).
    DescriptorHandle<Texture2D<float4>> ratios;
    DescriptorHandle<Texture2D<float>> distances;  ///< the pixel's distance from the camera (m, r16_float)
    uint2 extent;  ///< the cell textures' size
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
    uint pad4;
    uint pad5;
};

/// Azimuth k's direction in the map's frame: cos and sin of k * 45 degrees.
static const float2 sky_azimuth[8] = { float2(1.0, 0.0),        float2(0.70710678, 0.70710678),
                                       float2(0.0, 1.0),        float2(-0.70710678, 0.70710678),
                                       float2(-1.0, 0.0),       float2(-0.70710678, -0.70710678),
                                       float2(0.0, -1.0),       float2(0.70710678, -0.70710678) };

// ---- the shading's side -----------------------------------------------------------------------------------------

/// The share of the sky's ambient light the scene leaves a shaded pixel.
struct SkyOcclusionSample {
    float3 diffuse;
    float specular;
    float fade;  ///< 1: the occlusion applies fully; 0: the open sky (beyond its reach, or off)
};

/// No occlusion: the open sky over open ground.
SkyOcclusionSample sky_occlusion_open() {
    SkyOcclusionSample o;
    o.diffuse = float3(1.0, 1.0, 1.0);
    o.specular = 1.0;
    o.fade = 0.0;
    return o;
}

/// The occlusion at full-resolution pixel position `pixel` (its center), whose surface is `distance` meters from the
/// camera: the 4 cells around it (cell i stands for full-resolution pixel sky_cell * i + sky_cell / 2), bilinear,
/// each by how close its distance is to this one's (within 5 %); with none close, the closest. The ratios are already
/// faded out with distance.
SkyOcclusionSample sky_occlusion_at(SkyOcclusionFrame f, float2 pixel, float distance) {
    SkyOcclusionSample o = sky_occlusion_open();
    o.fade = 1.0 - smoothstep(f.reach.x, f.reach.y, distance);
    if (o.fade <= 0.0) {
        o.fade = 0.0;
        return o;
    }
    const float2 g = (pixel - (0.5 * float(sky_cell) + 0.5)) / float(sky_cell);
    const float2 t = g - floor(g);
    const int2 base = int2(floor(g));
    const int2 last = int2(f.extent) - 1;
    float closest = 1e30;
    float4 nearest = float4(1.0, 1.0, 1.0, 1.0);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight = 0.0;
    [unroll]
    for (uint i = 0; i < 4; ++i) {
        const int2 at = clamp(base + int2(int(i & 1u), int(i >> 1)), int2(0, 0), last);
        const float4 ratio = f.ratios.Load(int3(at, 0));
        const float difference = abs(f.distances.Load(int3(at, 0)) - distance);
        const float bilinear = ((i & 1u) != 0 ? t.x : 1.0 - t.x) * ((i >> 1) != 0 ? t.y : 1.0 - t.y);
        const float w = (bilinear + 1e-3) * saturate(1.0 - difference / (0.05 * distance));
        sum += w * ratio;
        weight += w;
        if (difference < closest) {
            closest = difference;
            nearest = ratio;
        }
    }
    const float4 ratio = weight > 1e-4 ? sum / weight : nearest;
    o.diffuse = ratio.rgb;
    o.specular = ratio.a;
    return o;
}

// ---- the trace's side ---------------------------------------------------------------------------------------------

/// What the trace found in one azimuth.
struct SkyArc {
    float lo;  ///< the hidden arc's elevations (radians, 0 to pi / 2); lo == hi: nothing hidden
    float hi;
    float2 lit_occluder;  ///< per light slot: whether what hides the sky there is sunlit (0 or 1)
    float2 lit_ground;    ///< per light slot: the sunlit share of the ground there
};

/// What the ambient table's evaluation needs from the occlusion, for one surface (sky_ambient_occluded).
struct SkyOcclusionTerms {
    float blocked;  ///< the cosine-weighted share of the normal's sphere the scene hides above the horizontal, / pi
    float below;    ///< the cosine-weighted share of the normal's sphere below the horizontal (the ground), / pi
    /// Per azimuth's sector, cosine-weighted measures (/ pi) and the middle elevations of the parts they weigh, in
    /// pairs (x, y and z, w): the open sky in the normal's hemisphere in two halves; the sky the scene leaves visible
    /// (below the hidden arc and above it); the ground in two halves. Halves follow the sky's change with elevation
    /// (the glow at the horizon near a low sun). The same quadrature with and without the occlusion, so that what it
    /// gets wrong cancels in their ratio.
    float4 sky[8];
    float4 visible[8];
    float4 ground[8];
    float3 up;  ///< the azimuths' frame (SkyOcclusionFrame)
    float3 axis_x;
    float3 axis_y;
    float2 lit_blocked;      ///< per light slot: the sunlit share of what hides the sky
    float2 lit_below;        ///< per light slot: the sunlit share of the ground
    float specular_blocked;  ///< the share of the specular lobe the scene hides above the horizontal
    float specular_below;    ///< the share of the specular lobe below the horizontal
    float4 lights[2];        ///< SkyOcclusionFrame::lights
    /// Per sun of the sky that no shadow map follows: whether the height map leaves it in sight of the point, the
    /// sunlit share then of everything the point sees.
    float4 sun_visible;
};

/// The integral of (a cos t + b sin t) cos t dt from t0 to t1 (0 when t1 <= t0): the cosine-weighted measure of an arc
/// of elevations in one azimuth, per radian of azimuth.
float sky_arc_integral(float a, float b, float t0, float t1) {
    if (t1 <= t0) {
        return 0.0;
    }
    const float s0 = sin(t0);
    const float c0 = cos(t0);
    const float s1 = sin(t1);
    const float c1 = cos(t1);
    return 0.5 * a * ((t1 - t0) + s1 * c1 - s0 * c0) + 0.5 * b * (s1 * s1 - s0 * s0);
}

/// Band attenuations of the lobe each sector's hidden sky and ground are read with: a von Mises-Fisher lobe of mean
/// resultant length 0.9, about the spread of a 45-degree sector's arc.
static const float3 sky_sector_lobe = float3(1.0, 0.904, 0.739);

/// The world direction at elevation `elevation` in azimuth k of the terms' frame.
float3 sky_sector_direction(SkyOcclusionTerms t, uint k, float elevation) {
    const float3 horizontal = sky_azimuth[k].x * t.axis_x + sky_azimuth[k].y * t.axis_y;
    return cos(elevation) * horizontal + sin(elevation) * t.up;
}

/// The occlusion's terms for a surface of normal `n` whose specular lobe points along `dominant` with GGX roughness
/// `alpha`, from the 8 azimuths' arcs.
SkyOcclusionTerms sky_occlusion_terms(SkyOcclusionFrame f, SkyArc arcs[8], float3 n, float3 dominant, float alpha) {
    SkyOcclusionTerms t;
    t.lights[0] = f.lights[0];
    t.lights[1] = f.lights[1];
    t.sun_visible = float4(1.0, 1.0, 1.0, 1.0);
    t.up = f.up.xyz;
    t.axis_x = f.axis_x.xyz;
    t.axis_y = f.axis_y.xyz;
    const float n_up = dot(n, t.up);
    const float2 n_h = float2(dot(n, t.axis_x), dot(n, t.axis_y));
    const float sector = sky_pi / 4.0;
    // The reflection's azimuth, between two of the eight.
    float azimuth = atan2(dot(dominant, t.axis_y), dot(dominant, t.axis_x)) / sector;
    azimuth = azimuth < 0.0 ? azimuth + 8.0 : azimuth;
    const uint r0 = uint(floor(azimuth)) % sky_azimuths;
    const uint r1 = (r0 + 1u) % sky_azimuths;
    const float r_share = azimuth - floor(azimuth);
    float spec_lo = 0.0;
    float spec_hi = 0.0;
    float blocked = 0.0;
    float below = 0.0;
    float2 lit_blocked = float2(0.0, 0.0);
    float2 lit_below = float2(0.0, 0.0);
    for (uint k = 0; k < sky_azimuths; ++k) {
        const SkyArc arc = arcs[k];
        // The normal's hemisphere in this azimuth's vertical half-plane: elevations within 90 degrees of delta.
        const float a = dot(n_h, sky_azimuth[k]) * sky_sector_sinc;
        const float delta = atan2(n_up, a);
        const float t0 = max(arc.lo, delta - sky_half_pi);
        const float t1 = min(arc.hi, delta + sky_half_pi);
        const float g0 = max(-sky_half_pi, delta - sky_half_pi);
        const float g1 = min(0.0, delta + sky_half_pi);
        const float s0 = max(0.0, delta - sky_half_pi);
        const float s1 = min(sky_half_pi, delta + sky_half_pi);
        const float sm = 0.5 * (s0 + s1);
        const float gm = 0.5 * (g0 + g1);
        const float m = sky_arc_integral(a, n_up, t0, t1) * sector;
        const float h0 = sky_arc_integral(a, n_up, g0, gm) * sector;
        const float h1 = sky_arc_integral(a, n_up, gm, g1) * sector;
        const float g = h0 + h1;
        // The visible sky: under the hidden arc and over it (all of it when nothing is hidden).
        const bool hides = arc.hi > arc.lo;
        const float v0 = hides ? min(arc.lo, s1) : sm;
        const float v1 = hides ? max(arc.hi, s0) : sm;
        t.sky[k] = float4(sky_arc_integral(a, n_up, s0, sm) * sector / sky_pi, 0.5 * (s0 + sm),
                          sky_arc_integral(a, n_up, sm, s1) * sector / sky_pi, 0.5 * (sm + s1));
        t.visible[k] = float4(sky_arc_integral(a, n_up, s0, v0) * sector / sky_pi, 0.5 * (s0 + v0),
                              sky_arc_integral(a, n_up, v1, s1) * sector / sky_pi, 0.5 * (v1 + s1));
        t.ground[k] = float4(h0 / sky_pi, 0.5 * (g0 + gm), h1 / sky_pi, 0.5 * (gm + g1));
        blocked += m;
        below += g;
        lit_blocked += m * arc.lit_occluder;
        lit_below += g * arc.lit_ground;
        if (k == r0) {
            spec_lo += (1.0 - r_share) * arc.lo;
            spec_hi += (1.0 - r_share) * arc.hi;
        }
        if (k == r1) {
            spec_lo += r_share * arc.lo;
            spec_hi += r_share * arc.hi;
        }
    }
    t.blocked = blocked / sky_pi;
    t.below = below / sky_pi;
    t.lit_blocked = blocked > 1e-6 ? lit_blocked / blocked : float2(1.0, 1.0);
    t.lit_below = below > 1e-6 ? lit_below / below : float2(1.0, 1.0);
    // The specular lobe as an arc of elevations around the reflection, as wide as two GGX alphas each way.
    const float elevation = asin(clamp(dot(dominant, t.up), -1.0, 1.0));
    const float width = clamp(2.0 * alpha, 0.05, sky_half_pi);
    const float lobe_lo = elevation - width;
    const float lobe_hi = elevation + width;
    t.specular_blocked =
        spec_hi > spec_lo ? max(min(lobe_hi, spec_hi) - max(lobe_lo, spec_lo), 0.0) / (2.0 * width) : 0.0;
    t.specular_below = saturate((min(lobe_hi, 0.0) - lobe_lo) / (2.0 * width));
    return t;
}

/// The slot of the shadowed light toward `l`, or 2 if none shadows it.
uint sky_light_slot(SkyOcclusionTerms t, float3 l) {
    if (t.lights[0].w > 0.0 && dot(t.lights[0].xyz, l) > 0.9999) {
        return 0;
    }
    if (t.lights[1].w > 0.0 && dot(t.lights[1].xyz, l) > 0.9999) {
        return 1;
    }
    return 2;
}
