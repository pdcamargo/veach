// TerrainMaterial (docs/specs/terrain-materials/spec.md, ADR 0098): up to six surface layers per body, blended per
// pixel. An app registers it with veach_add_terrain_material(<target>); runtime::TerrainSurface makes the records.
//
// The mesh: terrain chunks carry each vertex's six layer weights in its tangent (layers 0-3) and uv (layers 4-5),
// computed on the CPU from the body's surface rules (slope, altitude, depth, climate) and interpolated across the
// triangle. Per pixel the weights are sharpened by the layers' heights (height blending), and each layer is sampled
// triplanar in the chunk's object space (body axes; chunk origins are multiples of 16 m, so tile sizes that divide
// 16 m continue across chunks) at two scales, near and far, faded by the pixel's footprint; past both, the layer's
// mean color and roughness stand in, so the planet from orbit shows the same colors as its ground. A low-frequency
// noise in the body's frame (SurfaceInput::material_origin, wrapped at 65,536 m) varies brightness and roughness.
//
// The record:
//   array_texture0   albedo (sRGB rgb) and height (a), an array layer per library layer
//   array_texture1   normal (rg: -dh/du and -dh/dv, 0.5 = flat), roughness (b), AO (a)
//   normal_texture   Texture2D<float4>, rgba32f: the layer table, row k for slot k, row 6 the globals
//   custom0          x layer count, y macro strength, z height-blend strength, w triplanar sharpness
// Row 6: [0] x fade start, y fade end (footprint / tile), z AO strength, w normal scale
// Each slot's row: [0] near tile m, far tile m, array layer, mode (0 scanned color, 1 palette color)
//           [1] mean color (linear rgb, of what the mode paints), mean luminance of the scan
//           [2] palette color (linear rgb), palette detail contrast
//           [3] mean roughness, roughness scale, mean height, normal strength

import veach.material;

static const uint terrain_slots = 6;
static const float terrain_period = 65536.0;  ///< material_origin's wrap: the macro noise's period

struct TerrainLayerRow {
    float4 r0;
    float4 r1;
    float4 r2;
    float4 r3;
};

TerrainLayerRow terrain_row(MaterialContext c, uint slot) {
    const Texture2D<float4> table = nonuniform(c.record.normal_texture);
    TerrainLayerRow r;
    r.r0 = table.Load(int3(0, int(slot), 0));
    r.r1 = table.Load(int3(1, int(slot), 0));
    r.r2 = table.Load(int3(2, int(slot), 0));
    r.r3 = table.Load(int3(3, int(slot), 0));
    return r;
}

Texture2DArray<float4> terrain_albedo(MaterialContext c) {
    return nonuniform(c.record.array_texture0);
}
Texture2DArray<float4> terrain_detail(MaterialContext c) {
    return nonuniform(c.record.array_texture1);
}

// ---- macro noise: value noise on a lattice wrapped at terrain_period, so it continues where material_origin wraps

uint terrain_hash(uint3 p) {
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u ^ p.z * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

float terrain_value_noise(float3 p, float cell) {
    const float3 q = p / cell;
    const float3 f = floor(q);
    const float3 t = q - f;
    const float3 s = t * t * (3.0 - 2.0 * t);
    const uint wrap = uint(terrain_period / cell);  // cells are powers of two: an exact divisor
    const int3 i = int3(f);
    float v[8];
    for (uint k = 0; k < 8; ++k) {
        const int3 corner = i + int3(int(k & 1u), int((k >> 1) & 1u), int((k >> 2) & 1u));
        const uint3 wrapped = uint3((corner % int(wrap) + int(wrap)) % int(wrap));
        v[k] = float(terrain_hash(wrapped) & 0xFFFFu) / 32767.5 - 1.0;
    }
    const float x0 = lerp(lerp(v[0], v[1], s.x), lerp(v[2], v[3], s.x), s.y);
    const float x1 = lerp(lerp(v[4], v[5], s.x), lerp(v[6], v[7], s.x), s.y);
    return lerp(x0, x1, s.z);
}

// ---- triplanar sampling ---------------------------------------------------------------------------------------

struct TerrainTexel {
    float4 albedo_height;  ///< linear rgb, height
    float4 detail;         ///< tangent normal xy (-1..1), roughness, AO
};

/// Projection `axis` (0 x, 1 y, 2 z) of a position: its two other coordinates.
float2 terrain_project(float3 p, uint axis) {
    return axis == 0 ? p.zy : axis == 1 ? p.xz : p.xy;
}

/// The two arrays at one projection: uv = projected position / tile, with its pixel gradients.
/// Without `with_detail` only the albedo array is read (flat normal, no AO; the caller ignores the roughness).
TerrainTexel terrain_fetch(MaterialContext c, uint axis, float tile, float layer, float2 offset, bool filtered,
                           bool with_detail) {
    const float2 uv = terrain_project(c.surface.object_position, axis) / tile + offset;
    const float3 coords = float3(uv, layer);
    TerrainTexel t;
    t.detail = float4(0.5, 0.5, 0.0, 1.0);
    if (filtered) {
        const float2 dx = terrain_project(c.surface.object_dx, axis) / tile;
        const float2 dy = terrain_project(c.surface.object_dy, axis) / tile;
        t.albedo_height = terrain_albedo(c).SampleGrad(nonuniform(c.record.sampler), coords, dx, dy);
        if (with_detail) {
            t.detail = terrain_detail(c).SampleGrad(nonuniform(c.record.sampler), coords, dx, dy);
        }
    } else {
        t.albedo_height = terrain_albedo(c).SampleLevel(nonuniform(c.record.sampler), coords, 0.0);
        if (with_detail) {
            t.detail = terrain_detail(c).SampleLevel(nonuniform(c.record.sampler), coords, 0.0);
        }
    }
    t.detail.xy = t.detail.xy * 2.0 - 1.0;
    return t;
}

/// One scale of one layer, triplanar: color, height, roughness and AO blended by the projection weights, and the
/// object-space normal perturbation (UDN: each projection's xy along its own tangent axes).
struct TerrainScale {
    float3 albedo;
    float height;
    float roughness;
    float ao;
    float3 bump;  ///< object space, to add to the vertex normal
};

TerrainScale terrain_scale(MaterialContext c, float3 projection, float tile, float layer, float2 offset,
                           bool filtered, bool with_detail) {
    TerrainScale s;
    s.albedo = float3(0.0, 0.0, 0.0);
    s.height = 0.0;
    s.roughness = 0.0;
    s.ao = 0.0;
    s.bump = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint axis = 0; axis < 3; ++axis) {
        const float w = projection[axis];
        if (w <= 0.0) {
            continue;
        }
        const TerrainTexel t = terrain_fetch(c, axis, tile, layer, offset, filtered, with_detail);
        s.albedo += t.albedo_height.rgb * w;
        s.height += t.albedo_height.a * w;
        s.roughness += t.detail.b * w;
        s.ao += t.detail.a * w;
        // The projection's tangent axes: u and v are the projected coordinates (terrain_project).
        const float3 tu = axis == 0 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
        const float3 tv = axis == 1 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
        s.bump += (tu * t.detail.x + tv * t.detail.y) * w;
    }
    return s;
}

float terrain_luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

struct TerrainMaterial : IMaterial {
    override float4 evalBaseColor(MaterialContext c) { return float4(1.0, 1.0, 1.0, 1.0); }  // always opaque

    override SurfaceSample evalSurface(MaterialContext c) {
        const SurfaceInput s = c.surface;
        const uint count = min(uint(c.record.custom0.x), terrain_slots);
        const float macro_strength = c.record.custom0.y;
        const float height_blend = c.record.custom0.z;
        const float sharpness = c.record.custom0.w;
        const float4 globals = nonuniform(c.record.normal_texture).Load(int3(0, int(terrain_slots), 0));
        const float fade_start = globals.x;
        const float fade_end = globals.y;
        const float ao_strength = globals.z;
        const float normal_scale = globals.w;

        // The pixel's footprint in meters; 0 (the path tracer) samples mip 0 everywhere.
        const bool filtered = any(s.object_dx != 0.0) || any(s.object_dy != 0.0);
        const float footprint = filtered ? max(length(s.object_dx), length(s.object_dy)) : 0.0;

        float weights[terrain_slots] = { s.object_tangent.x, s.object_tangent.y, s.object_tangent.z,
                                         s.object_tangent.w, s.uv.x, s.uv.y };

        // Projection weights from the object normal: sharpened so most ground reads one or two projections.
        float3 projection = pow(abs(s.object_normal), float3(sharpness, sharpness, sharpness));
        projection /= max(projection.x + projection.y + projection.z, 1e-6);
        projection = max(projection - 0.08, 0.0);
        projection /= max(projection.x + projection.y + projection.z, 1e-6);

        // The macro variation, in the body's frame (wrapped at terrain_period).
        const float3 macro_p = s.object_position + s.material_origin;
        const float macro = terrain_value_noise(macro_p, 128.0) * 0.55 + terrain_value_noise(macro_p, 2048.0) * 0.45;

        // Each layer's surface at this pixel, then the height blend.
        float3 albedo[terrain_slots];
        float height[terrain_slots];
        float roughness[terrain_slots];
        float ao[terrain_slots];
        float3 bump[terrain_slots];
        float best = -1e9;
        float detail = 0.0;  ///< how much of the layers' textures the pixel sees (0: only their means, far away)
        for (uint k = 0; k < terrain_slots; ++k) {
            albedo[k] = float3(0.0, 0.0, 0.0);
            height[k] = 0.0;
            roughness[k] = 1.0;
            ao[k] = 1.0;
            bump[k] = float3(0.0, 0.0, 0.0);
            weights[k] = k < count ? max(weights[k], 0.0) : 0.0;
            if (weights[k] < 0.004) {
                weights[k] = 0.0;
                continue;
            }
            const TerrainLayerRow row = terrain_row(c, k);
            const float near_tile = row.r0.x;
            const float far_tile = row.r0.y;
            const float layer = row.r0.z;
            const bool palette = row.r0.w > 0.5;
            const float3 mean = row.r1.rgb;
            const float mean_luminance = max(row.r1.w, 1e-3);
            // Detail fades out by the footprint relative to each scale's tile: past it the tiles would repeat
            // visibly and the texels average to the mean anyway.
            const float near_w = filtered ? 1.0 - smoothstep(fade_start, fade_end, footprint / near_tile) : 1.0;
            const float far_w = filtered ? 1.0 - smoothstep(fade_start, fade_end, footprint / far_tile) : 1.0;
            const float2 offset = float2(0.37, 0.61) * float(k);
            float3 color = mean;
            float h = row.r3.z;
            float r = row.r3.x;
            float occlusion = 1.0;
            float3 b = float3(0.0, 0.0, 0.0);
            if (far_w > 0.0) {
                // Where the near tile shows fully, the far one only varies its color: its detail array is not read.
                const TerrainScale far =
                    terrain_scale(c, projection, far_tile, layer, offset * 0.5, filtered, near_w < 0.999);
                color = lerp(mean, far.albedo, far_w);
                h = lerp(h, far.height, far_w);
                r = lerp(r, far.roughness * row.r3.y, far_w);
                occlusion = lerp(1.0, far.ao, far_w);
                b = far.bump * far_w;
                if (near_w > 0.0) {
                    const TerrainScale near =
                        terrain_scale(c, projection, near_tile, layer, offset, filtered, true);
                    // The near tile, varied by the far one's departure from the mean: the near pattern no
                    // longer repeats identically.
                    const float3 variation = clamp(color / max(mean, 1e-3), 0.6, 1.6);
                    color = lerp(color, near.albedo * variation, near_w);
                    h = lerp(h, near.height * 0.7 + h * 0.3, near_w);
                    r = lerp(r, near.roughness * row.r3.y, near_w);
                    occlusion = lerp(occlusion, near.ao * lerp(1.0, far.ao, 0.5), near_w);
                    b = lerp(b, near.bump + far.bump * 0.35, near_w);
                }
            }
            if (palette) {
                // The scan's light and shade, the palette's color (the opt-in procedural albedo).
                const float detail = pow(max(terrain_luminance(color) / mean_luminance, 0.0), row.r2.w);
                color = row.r2.rgb * clamp(detail, 0.2, 3.0);
            }
            albedo[k] = color;
            height[k] = h;
            roughness[k] = r;
            ao[k] = occlusion;
            bump[k] = b * row.r3.w;
            best = max(best, weights[k] + h * height_blend);
            detail = max(detail, far_w);
        }

        // Height blending: a layer shows where its weight plus its height is near the best; sand fills the cracks
        // between stones instead of fading into them.
        // Far away the heights are only the layers' means: there the weights blend as they are, or the borders would
        // follow the coarse chunks' triangles.
        const float depth = 0.12;
        float sharp_total = 0.0;
        float sharp[terrain_slots];
        for (uint k = 0; k < terrain_slots; ++k) {
            sharp[k] = weights[k] > 0.0 ? max(weights[k] + height[k] * height_blend - (best - depth), 0.0) : 0.0;
            sharp_total += sharp[k];
        }
        float total = 0.0;
        for (uint k = 0; k < terrain_slots; ++k) {
            weights[k] = lerp(weights[k], sharp_total > 0.0 ? sharp[k] / sharp_total : 0.0, detail);
            total += weights[k];
        }
        float3 out_albedo = float3(0.0, 0.0, 0.0);
        float out_roughness = 0.0;
        float out_ao = 0.0;
        float3 out_bump = float3(0.0, 0.0, 0.0);
        const float norm = total > 0.0 ? 1.0 / total : 0.0;
        for (uint k = 0; k < terrain_slots; ++k) {
            const float w = weights[k] * norm;
            out_albedo += albedo[k] * w;
            out_roughness += roughness[k] * w;
            out_ao += ao[k] * w;
            out_bump += bump[k] * w;
        }
        if (total <= 0.0) {
            out_albedo = float3(0.5, 0.5, 0.5);
            out_roughness = 0.9;
            out_ao = 1.0;
        }
        out_albedo *= 1.0 + macro_strength * macro;
        out_albedo *= lerp(1.0, out_ao, ao_strength);

        SurfaceSample result;
        result.base_color = saturate(out_albedo);
        result.alpha = 1.0;
        result.metallic = 0.0;
        result.roughness = saturate(out_roughness + macro_strength * 0.3 * macro);
        result.emission = float3(0.0, 0.0, 0.0);
        const float3 n_object = normalize(s.object_normal + out_bump * normal_scale);
        float3 n = normalize(mul(s.object_to_world, n_object));
        // A bump must not tip the shading normal below the surface the viewer sees.
        result.normal = dot(n, s.geometric_normal) > 0.0 ? n : normalize(s.normal);
        return result;
    }
};
