// Sun shadows in the real-time renderer (docs/specs/sun-shadows/spec.md, ADR 0062, ADR 0095): cascaded shadow maps for
// the brightest directional lights, drawn by shadows.cpp through the GPU-driven culling, looked up here by the shading
// function. Each shadowed light has up to four cascades in one depth32 2D array (layer = slot x cascade_count + cascade),
// each an orthographic, reverse-Z view (depth 1 toward the light) fitted around a slice of the camera's frustum and
// snapped to its texels; a pixel uses the cascade of its view depth, the last tenth of each blending into the next. Filtering: a blocker search sizes a comparison-sampler PCF kernel by the light's angular radius
// (soft shadows that harden at contact), at least a texel wide. Layout matches render::ShadowFrame (shadows.hpp).

import veach.bindless;

static const uint max_shadowed_lights = 2;
static const uint max_shadow_cascades = 4;

/// Mirrors render::ShadowCascade.
struct ShadowCascade {
    float4x4 clip_from_render;  ///< render space to the cascade's clip space (orthographic, reverse-Z)
    float4 scale;               ///< x: texel size (m); y: meters per unit of depth; z: radius (m); w: unused
};

/// Mirrors render::ShadowFrame: this frame's shadowed lights and their cascades.
struct ShadowFrame {
    ShadowCascade cascades[max_shadowed_lights * max_shadow_cascades];  ///< by layer of the map
    DescriptorHandle<Texture2DArray<float>> map;
    DescriptorHandle<SamplerComparisonState> compare;  ///< greater_equal, linear: 2x2 PCF per tap
    uint light_count;                                  ///< 0: nothing casts
    uint cascade_count;
    uint2 lights;          ///< the LightRecord index of each shadowed light
    float4 view_axis;      ///< the camera's forward; w: minus its dot with the camera's position
    float4 splits;         ///< each cascade's far view depth (m)
    float fade_start;      ///< meters from the camera where the last cascade starts fading out...
    float fade_end;        ///< ...to unshadowed
    float resolution;      ///< texels per side
    float normal_offset;   ///< texels along the surface's normal, at grazing light
    float depth_bias;      ///< texels toward the light
    float max_penumbra;    ///< texels: the blocker search's radius and the widest filter
    float blend;           ///< the share of a cascade's far depth over which it blends into the next
    float pad;
};

static const float2 shadow_disk[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),  float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760),   float2(-0.91588581, 0.45771432),  float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),   float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420),  float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590),  float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790)
};

/// The lit fraction of one cascade at p: a blocker search over max_penumbra texels, then PCF over the penumbra the
/// light's angular radius gives the average blocker's distance (at least one texel).
float cascade_shadow(ShadowFrame f, uint layer, ShadowCascade c, float3 p, float3 n, float3 l, float cos_l,
                     float light_radius) {
    const float texel = c.scale.x;
    const float sin_l = sqrt(saturate(1.0 - cos_l * cos_l));
    // Normal offset (more at grazing light, where acne lives) and a constant bias toward the light, both in texels.
    const float3 q = p + n * (f.normal_offset * texel * sin_l) + l * (f.depth_bias * texel);
    const float4 clip = mul(c.clip_from_render, float4(q, 1.0));
    const float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    const float depth = clip.z;
    if (depth >= 1.0 || depth <= 0.0 || any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0))) {
        return 1.0;  // outside the cascade (beyond its near plane, toward the light, nothing in the map is nearer)
    }
    const float size = f.resolution;
    const float search = f.max_penumbra;
    // Blockers: texels nearer the light than the receiver (reverse-Z: greater), on a 4x4 grid.
    float blocker_sum = 0.0;
    float blockers = 0.0;
    const float2 center = uv * size - 0.5;
    [unroll]
    for (uint i = 0; i < 16; ++i) {
        const float2 offset = (float2(float(i % 4), float(i / 4)) - 1.5) * (search / 1.5);
        const int2 t = clamp(int2(floor(center + offset + 0.5)), int2(0, 0), int2(int(size) - 1, int(size) - 1));
        const float z = f.map.Load(int4(t, int(layer), 0));
        if (z > depth) {
            blocker_sum += z;
            blockers += 1.0;
        }
    }
    if (blockers == 0.0) {
        return 1.0;
    }
    const float gap = (blocker_sum / blockers - depth) * c.scale.y;  // meters from the receiver to the blockers
    const float penumbra = clamp(gap * tan(light_radius) / texel, 1.5, search);
    float lit = 0.0;
    [unroll]
    for (uint i = 0; i < 16; ++i) {
        const float2 at = uv + shadow_disk[i] * (penumbra / size);
        lit += f.map.SampleCmpLevelZero(f.compare, float3(at, float(layer)), depth);
    }
    return lit / 16.0;
}

/// The fraction of light `light` (a LightRecord index) that reaches p, by the sun shadow maps: 1 for a light that
/// casts no shadow map, or beyond the last cascade. `n` faces the light (dot(n, l) = cos_l > 0), `l` points to it,
/// `distance` is p's distance from the camera (m), `light_radius` the light's angular radius (radians).
float light_shadow(ShadowFrame f, uint light, float3 p, float3 n, float3 l, float cos_l, float distance,
                   float light_radius) {
    if (f.light_count == 0 || distance >= f.fade_end) {
        return 1.0;
    }
    uint slot = max_shadowed_lights;
    if (light == f.lights.x) {
        slot = 0;
    } else if (f.light_count > 1 && light == f.lights.y) {
        slot = 1;
    }
    if (slot == max_shadowed_lights) {
        return 1.0;
    }
    // The cascade by view depth: each one's culling kept the casters of its own slice (shadows.cpp).
    const float depth = dot(f.view_axis.xyz, p) + f.view_axis.w;
    uint c = 0;
    while (c < f.cascade_count && depth > f.splits[c]) {
        ++c;
    }
    if (c == f.cascade_count) {
        return 1.0;
    }
    const uint layer = slot * f.cascade_count + c;
    float shadow = cascade_shadow(f, layer, f.cascades[layer], p, n, l, cos_l, light_radius);
    // The last share of a cascade blends into the next, so the change of resolution has no seam.
    const float band = f.splits[c] * f.blend;
    const float t = saturate((depth - (f.splits[c] - band)) / max(band, 1e-6));
    if (t > 0.0 && c + 1 < f.cascade_count) {
        shadow = lerp(shadow, cascade_shadow(f, layer + 1, f.cascades[layer + 1], p, n, l, cos_l, light_radius), t);
    }
    const float fade = saturate((distance - f.fade_start) / max(f.fade_end - f.fade_start, 1e-3));
    return lerp(shadow, 1.0, fade);
}
