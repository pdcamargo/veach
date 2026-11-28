// Screen-space ambient occlusion for the real-time renderer (ADR 0095, docs/specs/ambient-light/spec.md): GTAO
// (Jimenez et al. 2016) laid out as Intel's XeGTAO, from the visibility pass's depth alone, traced at half resolution.
//
//   ao_view_depth   the reverse-Z depth buffer as view distance (meters): mip 0 of a small pyramid;
//   ao_depth_mip    each further mip, a depth-weighted average of the one before (XeGTAO's filter), so samples far
//                   from their pixel read coarse mips and a large radius near the camera stays cheap;
//   ao_trace        per half-resolution pixel (the full-resolution pixel at twice its coordinates), the visible
//                   fraction of the hemisphere over the normal rebuilt from depth, from the horizons in `ao_slices`
//                   directions with `ao_steps` samples each way, and the depth edges to its 4 neighbors;
//   ao_denoise      an edge-aware 3 x 3 blur at half resolution, run twice, that never crosses a depth edge;
//   ao_upsample     to full resolution: the 4 nearest half-resolution values, weighted by how close their depth is to
//                   the pixel's.
//
// The slice directions and sample offsets follow a fixed 4 x 4 pattern: a still camera gives the same image every
// frame, and the blur averages the pattern out. No temporal reuse. Layout matches render::AoParams
// (ambient_occlusion.cpp).

import veach.bindless;

struct AoParams {
    /// x, y: view-space x and y per NDC unit at unit distance (1 / P00, 1 / P11); z = the near plane (m);
    /// w = the effect radius (m).
    float4 projection;
    /// x = where the falloff starts (fraction of the radius); y = the power on the visibility (the intensity);
    /// z = mips in the pyramid; w unused.
    float4 settings;
    /// ao_view_depth: the depth buffer. ao_depth_mip: a one-mip view of the mip before. ao_trace, ao_upsample: the
    /// whole pyramid. ao_denoise: the occlusion so far (its first channel).
    DescriptorHandle<Texture2D<float>> source;
    /// ao_denoise: ao_trace's output (occlusion, packed edges). ao_upsample: the half-resolution occlusion.
    DescriptorHandle<Texture2D<float2>> edges;
    DescriptorHandle<RWTexture2D<float>> target;  ///< ao_view_depth, ao_depth_mip (one mip), ao_denoise, ao_upsample
    DescriptorHandle<RWTexture2D<float2>> trace;  ///< ao_trace: visibility estimate, packed edges (rg16_float)
    uint2 size;    ///< the full-resolution view, pixels
    uint2 extent;  ///< this pass's target: the mip (ao_view_depth, ao_depth_mip), half (trace, denoise) or full
};
[[vk::push_constant]] ConstantBuffer<AoParams> params;

static const float ao_pi = 3.14159265358979;
static const float ao_sky = 1e18;  ///< the view distance of pixels with no geometry: finite, so no NaN downstream
static const uint ao_slices = 2;
static const uint ao_steps = 3;          ///< per side of each slice
static const float ao_mip_offset = 3.3;  ///< XeGTAO: a sample 2^(m + 3.3) pixels away reads mip m

// ---- the view-depth pyramid ------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void ao_view_depth(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    const float depth = params.source.Load(int3(id.xy, 0));  // reverse-Z, infinite far: near / distance
    params.target[id.xy] = depth > 0.0 ? min(params.projection.z / depth, ao_sky) : ao_sky;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ao_depth_mip(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    // The mip before is twice this one, rounded down at odd sizes: clamp to its last texel.
    const int2 last = int2(max(params.extent * 2, uint2(1, 1))) - 1;
    const int2 p = int2(id.xy) * 2;
    const float d0 = params.source.Load(int3(min(p, last), 0));
    const float d1 = params.source.Load(int3(min(p + int2(1, 0), last), 0));
    const float d2 = params.source.Load(int3(min(p + int2(0, 1), last), 0));
    const float d3 = params.source.Load(int3(min(p + int2(1, 1), last), 0));
    // XeGTAO's filter: an average that leaves out samples much nearer than the farthest, so a thin near edge does not
    // pull a wall forward.
    const float farthest = max(max(d0, d1), max(d2, d3));
    const float radius = 0.75 * params.projection.w;
    const float falloff_range = (1.0 - params.settings.x) * radius;
    const float falloff_mul = -1.0 / falloff_range;
    const float falloff_add = params.settings.x * radius / falloff_range + 1.0;
    const float4 d = float4(d0, d1, d2, d3);
    const float4 w = saturate((farthest - d) * falloff_mul + falloff_add);
    params.target[id.xy] = dot(w, d) / max(w.x + w.y + w.z + w.w, 1e-6);
}

// ---- the trace ---------------------------------------------------------------------------------------------------

/// View-space position (x right, y up, z forward, meters) of full-resolution pixel position `pixel` (y down) at
/// distance z.
float3 ao_position(float2 pixel, float z) {
    const float2 ndc = float2(pixel.x / float(params.size.x) * 2.0 - 1.0, 1.0 - pixel.y / float(params.size.y) * 2.0);
    return float3(ndc * params.projection.xy * z, z);
}

/// The pyramid's view distance at full-resolution pixel `pixel`, from mip `mip`.
float ao_depth(int2 pixel, uint mip) {
    const int2 last = int2(max(params.size >> mip, uint2(1, 1))) - 1;
    return params.source.Load(int3(clamp(pixel >> mip, int2(0, 0), last), int(mip)));
}

/// The view-space position the pyramid gives near full-resolution pixel position `pixel` at mip `mip`: the texel's
/// averaged distance at the texel's own center. At the pixel's position instead, a sloped plane would rise off itself
/// by up to half a texel's depth change and occlude its own surroundings.
float3 ao_sample(float2 pixel, uint mip) {
    const int2 last = int2(max(params.size >> mip, uint2(1, 1))) - 1;
    const int2 texel = clamp(int2(floor(pixel)) >> mip, int2(0, 0), last);
    const float z = params.source.Load(int3(texel, int(mip)));
    return ao_position((float2(texel) + 0.5) * float(1u << mip), z);
}

/// How connected the center is to its left, right, top and bottom neighbors (1: same surface, 0: a depth edge), from
/// their distances, forgiving a constant slope (XeGTAO).
float4 ao_edges(float center, float left, float right, float top, float bottom) {
    float4 edges = float4(left, right, top, bottom) - center;
    const float slope_lr = (edges.y - edges.x) * 0.5;
    const float slope_tb = (edges.w - edges.z) * 0.5;
    const float4 adjusted = edges + float4(slope_lr, -slope_lr, slope_tb, -slope_tb);
    edges = min(abs(edges), abs(adjusted));
    return saturate(1.25 - edges / (center * 0.022));  // XeGTAO's 0.011, doubled: neighbors are 2 pixels apart
}

float ao_pack_edges(float4 edges) {
    const float4 e = round(saturate(edges) * 2.9);
    return dot(e, float4(64.0, 16.0, 4.0, 1.0)) / 255.0;
}

float4 ao_unpack_edges(float packed) {
    const uint v = uint(packed * 255.0 + 0.5);
    return float4(float((v >> 6) & 3u), float((v >> 4) & 3u), float((v >> 2) & 3u), float(v & 3u)) / 3.0;
}

/// A 4 x 4 ordered pattern (Bayer): any 2 x 2 or 4 x 4 block holds evenly spread slice angles.
static const uint ao_bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

/// acos with an error under 0.07 degrees, for the horizon angles (Lagarde).
float ao_acos(float x) {
    const float a = abs(x);
    const float r = (-0.156583 * a + ao_pi * 0.5) * sqrt(1.0 - a);
    return x >= 0.0 ? r : ao_pi - r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ao_trace(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    const int2 pixel = int2(id.xy) * 2;  // the full-resolution pixel this one stands for
    const float2 center_pixel = float2(pixel) + 0.5;
    const float z = ao_depth(pixel, 0);
    const float z_left = ao_depth(pixel + int2(-2, 0), 0);
    const float z_right = ao_depth(pixel + int2(2, 0), 0);
    const float z_top = ao_depth(pixel + int2(0, -2), 0);
    const float z_bottom = ao_depth(pixel + int2(0, 2), 0);
    const float4 edges = ao_edges(z, z_left, z_right, z_top, z_bottom);
    const float radius = params.projection.w;
    // Meters per full-resolution pixel at this distance, and the radius in those pixels.
    const float pixel_size = z * 2.0 * params.projection.y / float(params.size.y);
    const float screen_radius = radius / pixel_size;
    if (z >= ao_sky * 0.5 || screen_radius < 2.0) {
        params.trace[id.xy] = float2(1.0, ao_pack_edges(edges));  // the sky, or a radius under a pixel: open
        return;
    }

    // The normal from the neighbors' positions, each pair weighted by how connected both are (XeGTAO).
    const float3 p = ao_position(center_pixel, z);
    const float3 to_left = normalize(ao_position(center_pixel + float2(-2.0, 0.0), z_left) - p);
    const float3 to_right = normalize(ao_position(center_pixel + float2(2.0, 0.0), z_right) - p);
    const float3 to_top = normalize(ao_position(center_pixel + float2(0.0, -2.0), z_top) - p);
    const float3 to_bottom = normalize(ao_position(center_pixel + float2(0.0, 2.0), z_bottom) - p);
    const float4 accepted =
        saturate(float4(edges.x * edges.z, edges.z * edges.y, edges.y * edges.w, edges.w * edges.x) + 0.01);
    float3 normal = accepted.x * cross(to_left, to_top) + accepted.y * cross(to_top, to_right) +
                    accepted.z * cross(to_right, to_bottom) + accepted.w * cross(to_bottom, to_left);
    const float3 view = normalize(-p);
    normal = normalize(normal);
    normal = dot(normal, view) < 0.0 ? -normal : normal;

    // Pushed slightly toward the camera so a flat surface does not occlude itself through depth precision.
    const float3 origin = p * 0.9992;
    const float falloff_range = (1.0 - params.settings.x) * radius;
    const float falloff_mul = -1.0 / falloff_range;
    const float falloff_add = params.settings.x * radius / falloff_range + 1.0;
    const uint index = ao_bayer[(id.y & 3u) * 4u + (id.x & 3u)];
    const float2 noise = float2((float(index) + 0.5) / 16.0, frac(0.5 + float(index) * 0.75487766624669276));
    const float min_s = 2.6 / screen_radius;  // never the pixel itself
    const uint mips = uint(params.settings.z);

    float visibility = 0.0;
    for (uint slice = 0; slice < ao_slices; ++slice) {
        const float phi = (float(slice) + noise.x) / float(ao_slices) * ao_pi;
        const float cos_phi = cos(phi);
        const float sin_phi = sin(phi);
        const float2 omega = float2(cos_phi, -sin_phi) * screen_radius;  // pixels, y down
        const float3 direction = float3(cos_phi, sin_phi, 0.0);
        const float3 ortho = direction - dot(direction, view) * view;
        const float3 axis = normalize(cross(ortho, view));
        const float3 projected = normal - axis * dot(normal, axis);
        const float projected_length = length(projected);
        const float sign_n = dot(ortho, projected) >= 0.0 ? 1.0 : -1.0;
        const float cos_n = saturate(dot(projected, view) / max(projected_length, 1e-6));
        const float n = sign_n * ao_acos(cos_n);
        const float sin_n = sign_n * sqrt(1.0 - cos_n * cos_n);
        // The horizons with nothing in the way: the tangent plane on each side.
        const float low_positive = -sin_n;  // cos(n + pi / 2)
        const float low_negative = sin_n;   // cos(n - pi / 2)
        float horizon_positive = low_positive;
        float horizon_negative = low_negative;
        for (uint step = 0; step < ao_steps; ++step) {
            const float step_noise = frac(noise.y + float(slice + step * ao_steps) * 0.6180339887498949);
            float s = (float(step) + step_noise) / float(ao_steps);
            s = s * s + min_s;
            const float2 offset = round(s * omega);
            const uint mip = uint(clamp(log2(max(length(offset), 1.0)) - ao_mip_offset, 0.0, float(mips - 1)));
            const float3 sample_positive = ao_sample(center_pixel + offset, mip) - origin;
            const float3 sample_negative = ao_sample(center_pixel - offset, mip) - origin;
            const float distance_positive = length(sample_positive);
            const float distance_negative = length(sample_negative);
            const float weight_positive = saturate(distance_positive * falloff_mul + falloff_add);
            const float weight_negative = saturate(distance_negative * falloff_mul + falloff_add);
            const float cos_positive = dot(sample_positive, view) / max(distance_positive, 1e-6);
            const float cos_negative = dot(sample_negative, view) / max(distance_negative, 1e-6);
            horizon_positive = max(horizon_positive, lerp(low_positive, cos_positive, weight_positive));
            horizon_negative = max(horizon_negative, lerp(low_negative, cos_negative, weight_negative));
        }
        // The visible arc between the horizons, cosine-weighted around the projected normal (the GTAO integral).
        // A side with nothing above its tangent plane takes the tangent exactly: through the approximate acos, an
        // open plane would land a hair inside or outside it by its angle to the view, and band.
        const float h0 = horizon_negative > low_negative ? max(-ao_acos(horizon_negative), n - ao_pi * 0.5)
                                                         : n - ao_pi * 0.5;
        const float h1 = horizon_positive > low_positive ? min(ao_acos(horizon_positive), n + ao_pi * 0.5)
                                                         : n + ao_pi * 0.5;
        const float arc0 = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n)) * 0.25;
        const float arc1 = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n)) * 0.25;
        visibility += lerp(projected_length, 1.0, 0.05) * (arc0 + arc1);
    }
    // Unclamped: a few slices of an open surface seen at an angle sum to more or less than 1, and only their average
    // over the pattern is 1. Clamping before the blur would darken such surfaces; the upsample clamps.
    visibility /= float(ao_slices);
    params.trace[id.xy] = float2(visibility, ao_pack_edges(edges));
}

// ---- the denoise, at half resolution -----------------------------------------------------------------------------

float ao_edges_at(int2 p, out float4 edges) {
    const bool inside = all(p >= 0) && all(p < int2(params.extent));
    edges = inside ? ao_unpack_edges(params.edges.Load(int3(p, 0)).y) : float4(0.0, 0.0, 0.0, 0.0);
    return inside ? params.source.Load(int3(p, 0)) : 1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ao_denoise(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    const int2 c = int2(id.xy);
    float4 e;
    float4 el;
    float4 er;
    float4 et;
    float4 eb;
    const float ao = ao_edges_at(c, e);
    const float ao_l = ao_edges_at(c + int2(-1, 0), el);
    const float ao_r = ao_edges_at(c + int2(1, 0), er);
    const float ao_t = ao_edges_at(c + int2(0, -1), et);
    const float ao_b = ao_edges_at(c + int2(0, 1), eb);
    // Edges made symmetric: both sides must agree they are connected.
    const float wl = e.x * el.y;
    const float wr = e.y * er.x;
    const float wt = e.z * et.w;
    const float wb = e.w * eb.z;
    // Diagonals through either neighbor.
    const float diagonal = 0.425;
    const float wtl = diagonal * (wl * el.z + wt * et.x);
    const float wtr = diagonal * (wr * er.z + wt * et.y);
    const float wbl = diagonal * (wl * el.w + wb * eb.x);
    const float wbr = diagonal * (wr * er.w + wb * eb.y);
    const int2 last = int2(params.extent) - 1;
    const float ao_tl = params.source.Load(int3(clamp(c + int2(-1, -1), int2(0, 0), last), 0));
    const float ao_tr = params.source.Load(int3(clamp(c + int2(1, -1), int2(0, 0), last), 0));
    const float ao_bl = params.source.Load(int3(clamp(c + int2(-1, 1), int2(0, 0), last), 0));
    const float ao_br = params.source.Load(int3(clamp(c + int2(1, 1), int2(0, 0), last), 0));
    const float center = 1.2;
    const float sum = center * ao + wl * ao_l + wr * ao_r + wt * ao_t + wb * ao_b + wtl * ao_tl + wtr * ao_tr +
                      wbl * ao_bl + wbr * ao_br;
    const float weight = center + wl + wr + wt + wb + wtl + wtr + wbl + wbr;
    params.target[id.xy] = sum / weight;
}

// ---- the upsample to full resolution -----------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void ao_upsample(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    const float z = ao_depth(int2(id.xy), 0);
    if (z >= ao_sky * 0.5) {
        params.target[id.xy] = 1.0;  // the sky
        return;
    }
    // Half-resolution pixel h stands for full-resolution pixel 2h: the 4 around this one, bilinear, each times how
    // close its depth is to this pixel's (within 2 % of the distance counts fully); with none close, the closest.
    const int2 base = int2(id.xy) >> 1;
    const float2 f = float2(id.xy & 1u) * 0.5;
    const int2 last = int2(params.size + 1u) / 2 - 1;
    float sum = 0.0;
    float weight = 0.0;
    float closest = 1.0;
    float closest_difference = 1e30;
    [unroll]
    for (uint i = 0; i < 4; ++i) {
        const int2 h = min(base + int2(int(i & 1u), int(i >> 1)), last);
        const float bilinear = ((i & 1u) != 0 ? f.x : 1.0 - f.x) * ((i >> 1) != 0 ? f.y : 1.0 - f.y);
        const float difference = abs(ao_depth(h * 2, 0) - z);
        const float ao = params.edges.Load(int3(h, 0)).x;
        const float w = (bilinear + 1e-3) * saturate(1.0 - difference / (0.02 * z));
        sum += w * ao;
        weight += w;
        if (difference < closest_difference) {
            closest_difference = difference;
            closest = ao;
        }
    }
    const float visibility = saturate(weight > 1e-4 ? sum / weight : closest);
    params.target[id.xy] = max(pow(visibility, params.settings.y), 0.03);
}
