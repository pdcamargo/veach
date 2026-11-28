// Large-scale sky occlusion (docs/specs/sky-occlusion/spec.md, veach/sky_occlusion.slang): the height map's heights
// and their pyramid, and the trace that reads them for every cell of 8 x 8 pixels.
//
//   sky_heights      the three depth maps from above (the highest surface, the lowest, the highest face turned down)
//                    as heights above the map's center along its up: x = the top, y = the top of the solid column
//                    from below, z = the bottom of the slab under the top, or sky_no_slab (sky_heights says which);
//   sky_heights_mip  each further mip: the highest top and column, the lowest slab bottom of the 4 texels below
//                    (conservative: a mip that mixes a column and a slab is a column);
//   sky_trace        per cell (full-resolution pixel 8i + 4), its point from the visibility
//                    depth, moved off its surface by a texel and a half along its triangle's normal; then in 8
//                    azimuths, 12 steps from 1 m to about 180 m, each read from the mip whose texels are about as
//                    long as the step: each column or slab marks the elevations it hides on a 32-bin mask of 0 to 90
//                    degrees, which becomes one arc of the same size and centroid. Three points in the air in front
//                    of what the point sees in each azimuth (the occluder, the ground 20 and 50 degrees down) are
//                    looked up in each shadowed light's shadow map. The ambient table is then evaluated with and
//                    without what is hidden (veach/ambient.slang, sky_ambient_occluded) and the ratio stored;
//   sky_blur         a 3 x 3 blur of the cells' ratios that never crosses a change of distance of more
//                    than 5 %: the trace's per-pixel decisions (which texel a step lands in, which shadow-map texel a
//                    sunlit test reads) vary from pixel to pixel on the same wall.
//
// Layout matches render::SkyParams (sky_occlusion.cpp).

import veach.ambient;
import veach.atmosphere;
import veach.bindless;
import veach.scene;  // InstanceRecord, the visible triangle's corners
import veach.shadows;
import veach.sky_occlusion;

struct SkyParams {
    float4x4 inverse_view_projection;  ///< sky_trace: the visibility pass's (jittered)
    float4 camera;                     ///< xyz: render space; w unused
    DescriptorHandle<StructuredBuffer<SkyOcclusionFrame>> frame;
    DescriptorHandle<StructuredBuffer<ShadowFrame>> shadows;  ///< sky_trace
    DescriptorHandle<StructuredBuffer<SkyFrame>> sky;         ///< sky_trace
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;  ///< sky_trace: the GPU scene
    DescriptorHandle<Texture2D<uint2>> visibility;                 ///< sky_trace: instance, meshlet << 7 | triangle
    /// sky_heights: the depth from above of the highest surface. sky_trace: the visibility depth (reverse-Z).
    DescriptorHandle<Texture2D<float>> top;
    DescriptorHandle<Texture2D<float>> lowest;    ///< sky_heights: the depth from above of the lowest surface
    DescriptorHandle<Texture2D<float>> downward;  ///< sky_heights: the depth from above of the highest face turned down
    DescriptorHandle<Texture2D<float4>> heights;  ///< sky_heights_mip: the mip before; sky_trace: every mip
    DescriptorHandle<RWTexture2D<float4>> heights_out;
    /// sky_trace: the ratios before the blur; sky_blur: after it (SkyOcclusionFrame::ratios).
    DescriptorHandle<RWTexture2D<float4>> ratios;
    DescriptorHandle<RWTexture2D<float>> distances;  ///< sky_trace: SkyOcclusionFrame::distances
    DescriptorHandle<Texture2D<float4>> raw;         ///< sky_blur: the trace's ratios
    DescriptorHandle<Texture2D<float>> near;         ///< sky_blur: the trace's distances
    uint2 size;    ///< the full-resolution view
    uint2 extent;  ///< this pass's target
};
[[vk::push_constant]] ConstantBuffer<SkyParams> params;

static const uint sky_steps = 12;
static const float sky_first_step = 1.0;  ///< meters: GTAO covers what is nearer
static const float sky_step_growth = 1.6;
static const uint sky_bins = 32;
/// A texel's slab bottom when it has no slab: above everything, so a mip's lowest slab bottom ignores it.
static const float sky_no_slab = 60000.0;

/// The elevation bin (of sky_bins over 0 to 90 degrees) of a rise of `slope` (height over distance, >= 0), through
/// an arctangent within 0.1 degree (Rajan et al.'s fit on [0, 1], mirrored above 1).
uint sky_bin(float slope) {
    const float x = slope <= 1.0 ? slope : 1.0 / slope;
    const float a = x * (0.7853982 + (1.0 - x) * (0.2447 + 0.0663 * x));
    const float angle = slope <= 1.0 ? a : sky_half_pi - a;
    return min(uint(angle * (float(sky_bins) / sky_half_pi)), sky_bins - 1);
}

/// Bits lo to hi (inclusive) of a 32-bit mask.
uint sky_bits(uint lo, uint hi) {
    const uint upper = hi >= 31u ? 0xFFFFFFFFu : (2u << hi) - 1u;
    return upper & ~((1u << lo) - 1u);
}
/// The roughness the specular ratio is measured for (GGX alpha): the shading applies it to every roughness.
static const float sky_specular_alpha = 0.25;

// ---- the heights ----------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void sky_heights(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    const SkyOcclusionFrame f = params.frame[0];
    // Reverse-Z from above: 1 at the map's top, 0 at its bottom. Nothing in the texel: the highest is still 0.
    const float highest = params.top.Load(int3(id.xy, 0));
    const float range = f.axis_x.w;
    const float bottom = -f.axis_y.w;
    if (highest <= 0.0) {
        params.heights_out[id.xy] = float4(bottom, bottom, sky_no_slab, 0.0);
        return;
    }
    const float top = highest * range + bottom;
    const float lowest = min(params.lowest.Load(int3(id.xy, 0)) * range + bottom, top);
    const float down = params.downward.Load(int3(id.xy, 0));
    const float downward = down > 0.0 ? down * range + bottom : bottom - 1.0;  // none: below everything
    // What is solid: a closed object's bottom faces down at its lowest surface, so it is a column up to its top; a
    // face turned down above the lowest surface is a ceiling, a slab down to it with the space under it open; with
    // no face turned down, one surface is the ground (or a shell whose roof is all there is), and a surface above
    // the lowest one is thin (an eave, an awning: single-sided), a slab of a quarter of the gap, 0.5 to 4 m.
    float column = lowest;
    float slab = sky_no_slab;
    if (top - lowest < 0.5 || (downward >= bottom && downward <= lowest + 0.25)) {
        column = top;
    } else if (downward > lowest + 0.25 && downward < top) {
        slab = downward;
    } else {
        slab = max(top - clamp(0.25 * (top - lowest), 0.5, 4.0), lowest);
    }
    params.heights_out[id.xy] = float4(top, column, slab, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void sky_heights_mip(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    // Conservative: the highest top and column, the lowest slab bottom of the 4 texels below.
    const int2 p = int2(id.xy) * 2;
    const float4 h0 = params.heights.Load(int3(p, 0));
    const float4 h1 = params.heights.Load(int3(p + int2(1, 0), 0));
    const float4 h2 = params.heights.Load(int3(p + int2(0, 1), 0));
    const float4 h3 = params.heights.Load(int3(p + int2(1, 1), 0));
    params.heights_out[id.xy] = float4(max(max(h0.xy, h1.xy), max(h2.xy, h3.xy)), min(min(h0.z, h1.z), min(h2.z, h3.z)),
                                       0.0);
}

// ---- the trace ---------------------------------------------------------------------------------------------------

/// Render-space position of full-resolution pixel `pixel` at reverse-Z depth `depth`.
float3 sky_position(int2 pixel, float depth) {
    const float2 uv = (float2(pixel) + 0.5) / float2(params.size);
    const float4 p = mul(params.inverse_view_projection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return p.xyz / p.w;
}

float sky_depth(int2 pixel) {
    return params.top.Load(int3(clamp(pixel, int2(0, 0), int2(params.size) - 1), 0));
}

/// Whether the air at `q` is in light `slot`'s sun: the shadow map of its view depth, one 2 x 2 comparison. Reads the
/// shadow frame's fields where they are (a copy of the whole frame would hold its 8 cascades in registers).
float sky_sunlit(uint slot, float3 q, float3 l) {
    if (slot >= params.shadows[0].light_count) {
        return 1.0;
    }
    const float4 axis = params.shadows[0].view_axis;
    const float depth = dot(axis.xyz, q) + axis.w;
    if (depth >= params.shadows[0].fade_end) {
        return 1.0;
    }
    const uint count = params.shadows[0].cascade_count;
    const float4 splits = params.shadows[0].splits;
    uint c = 0;
    while (c < count && depth > splits[c]) {
        ++c;
    }
    for (; c < count; ++c) {
        const uint layer = slot * count + c;
        const float4x4 clip_from_render = params.shadows[0].cascades[layer].clip_from_render;
        const float texel = params.shadows[0].cascades[layer].scale.x;
        const float4 clip = mul(clip_from_render, float4(q + l * (2.0 * texel), 1.0));
        const float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
        if (all(uv > float2(0.0, 0.0)) && all(uv < float2(1.0, 1.0)) && clip.z > 0.0 && clip.z < 1.0) {
            return params.shadows[0].map.SampleCmpLevelZero(params.shadows[0].compare, float3(uv, float(layer)), clip.z);
        }
    }
    return 1.0;
}

/// Whether the height map leaves direction `l` in sight from `start` (render space): a walk toward it in 10 steps out
/// to about 70 m, over columns and clear of slabs.
float sky_map_visible(SkyOcclusionFrame f, float3 start, float3 l) {
    const float rise = dot(l, f.up.xyz);
    if (rise <= 0.0) {
        return 0.0;  // below the horizontal: the ground, as the ambient table has it
    }
    const float2 run = float2(dot(l, f.axis_x.xyz), dot(l, f.axis_y.xyz));
    const float run_length = length(run);
    if (run_length < 1e-4) {
        return 1.0;  // straight up: nothing in a height map hangs over a column's own top
    }
    const float2 direction = run / run_length;
    const float slope = rise / run_length;
    const float3 o = start - f.center.xyz;
    const float2 at = float2(dot(o, f.axis_x.xyz), dot(o, f.axis_y.xyz));
    const float y = dot(o, f.up.xyz);
    const float texel = f.up.w;
    const float half_width = f.center.w;
    const int size = int(2.0 * half_width / texel + 0.5);
    const uint mips = uint(f.reach.z);
    float near = 0.5;
    float far = 1.0;
    for (uint step = 0; step < 10; ++step, near = far, far *= 1.6) {
        const float d = 0.5 * (near + far);
        const int2 t = int2(floor(float2(at.x + direction.x * d + half_width, half_width - at.y - direction.y * d) / texel));
        if (any(t < int2(0, 0)) || any(t >= int2(size, size))) {
            break;
        }
        const uint mip = uint(clamp(round(log2((far - near) / texel)), 0.0, float(mips - 1)));
        const float4 h = params.heights.Load(int3(t >> mip, int(mip)));
        // The ray's heights over the stretch this step stands for: under the column, or through the slab, hides it.
        const float low = y + slope * near;
        const float high = y + slope * far;
        if (low < h.y || (low < h.x && high > h.z)) {
            return 0.0;
        }
    }
    return 1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void sky_trace(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    const int2 pixel = min(int2(id.xy * sky_cell + sky_cell / 2), int2(params.size) - 1);
    const float depth = sky_depth(pixel);
    const SkyOcclusionFrame f = params.frame[0];
    const float3 p = sky_position(pixel, depth);
    const float distance = length(p - params.camera.xyz);
    const float fade = 1.0 - smoothstep(f.reach.x, f.reach.y, distance);
    // The sky, or beyond the reach: open. The sky's pixels as far as a half float goes, so no surface takes them.
    params.distances[id.xy] = depth > 0.0 ? min(distance, 65504.0) : 65504.0;
    const SkyFrame sky = params.sky[0];
    if (depth <= 0.0 || fade <= 0.0 || (sky.mode != sky_physical && sky.mode != sky_constant)) {
        params.ratios[id.xy] = float4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    // The visible triangle's own normal (the visibility buffer), facing the camera.
    const uint2 visible = params.visibility.Load(int3(pixel, 0));
    float3 n = f.up.xyz;
    if (visible.x != 0xFFFFFFFFu) {
        const InstanceRecord inst = params.instances[visible.x];
        const TriangleRef tri = meshlet_triangle(inst, visible.y >> 7, visible.y & 127u);
        const float3x3 corners = triangle_positions(inst, tri);
        const float3 p0 = to_world(inst.object_to_world, corners[0], 1.0);
        const float3 e = cross(to_world(inst.object_to_world, corners[1], 1.0) - p0,
                               to_world(inst.object_to_world, corners[2], 1.0) - p0);
        n = dot(e, e) > 1e-30 ? normalize(e) : n;
    }
    const float3 view = normalize(params.camera.xyz - p);
    n = dot(n, view) < 0.0 ? -n : n;

    // Off the surface by a texel and a half, so the surface's own texels of the map do not hide its sky.
    const float texel = f.up.w;
    const float3 start = p + n * (1.5 * texel + 0.1);
    const float3 o = start - f.center.xyz;
    const float2 at = float2(dot(o, f.axis_x.xyz), dot(o, f.axis_y.xyz));  // meters from the center
    const float y = dot(o, f.up.xyz);
    const uint mips = uint(f.reach.z);
    const float log2_texel = log2(texel);
    const float half_width = f.center.w;
    const int size = int(2.0 * half_width / texel + 0.5);
    const float3 up = f.up.xyz;

    // Under a slab at the point's own texel (a ceiling, a roof): the sky straight above is hidden in every azimuth,
    // from the slab's elevation half a texel away up to the zenith.
    uint above = 0;
    {
        const int2 own = int2(floor(float2(at.x + half_width, half_width - at.y) / texel));
        if (all(own >= int2(0, 0)) && all(own < int2(size, size))) {
            const float4 h = params.heights.Load(int3(own, 0));
            if (h.x > y && h.z > y && h.z < h.x) {
                above = sky_bits(sky_bin((h.z - y) / (0.5 * texel)), sky_bins - 1);
            }
        }
    }
    SkyArc arcs[8];
    for (uint k = 0; k < sky_azimuths; ++k) {
        const float2 direction = sky_azimuth[k];
        uint mask = above;
        float hit = 1e30;        // the near end of the first step that hid the horizon: a column (m)
        float hit_any = 1e30;    // ...or anything, a slab above too
        float ground = y - 2.0;  // the ground before it: the lowest column top seen (2 m down if none)
        bool ground_seen = false;
        float near = 0.5 * sky_first_step;
        float far = sky_first_step;
        for (uint step = 0; step < sky_steps; ++step, near = far, far *= sky_step_growth) {
            // Each step stands for the stretch since the one before: read at its middle, from the mip whose texels are
            // about as long, so a wall a texel thick between two steps is rarely skipped. (Coarser mips would catch
            // every wall, but their maximum over a wider area takes in the surface's own building behind it.)
            const float d = 0.5 * (near + far);
            const float2 s = at + direction * d;
            const uint mip = uint(clamp(round(log2(far - near) - log2_texel), 0.0, float(mips - 1)));
            // Texel rows run down the map's axis_y, as the rasterizer's rows run down clip space's y.
            const int2 t = int2(floor(float2(s.x + half_width, half_width - s.y) / texel));
            if (any(t < int2(0, 0)) || any(t >= int2(size, size))) {
                break;  // beyond the map: open
            }
            // x: the top; y: the solid column from below; z: the bottom of the slab under the top.
            const float4 h = params.heights.Load(int3(t >> mip, int(mip)));
            const float slab = h.z;
            const float column = slab <= y ? h.x : h.y;  // a slab reaching below the point hides the horizon too
            bool blocked = false;
            // Over the stretch from near to far, a column hides up to its top seen from near, a slab from its bottom
            // seen from far to its top seen from near: consecutive steps leave no gap between their elevations.
            const float inverse_near = 1.0 / near;
            const float inverse_far = 1.0 / far;
            if (column > y) {
                mask |= sky_bits(0u, sky_bin((column - y) * inverse_near));
                hit = min(hit, near);
                blocked = true;
            }
            if (h.x > y && slab > y && slab < h.x) {
                mask |= sky_bits(sky_bin((slab - y) * inverse_far), sky_bin((h.x - y) * inverse_near));
                hit_any = min(hit_any, near);
            }
            if (!blocked && hit > 1e29) {
                ground = ground_seen ? min(ground, h.y) : h.y;
                ground_seen = true;
            }
        }
        // One arc of the same size and centroid as the hidden bins.
        SkyArc arc;
        arc.lo = 0.0;
        arc.hi = 0.0;
        if (mask != 0) {
            const float count = float(countbits(mask));
            // The sum of the set bits' indices, a binary digit at a time.
            const float sum = float(countbits(mask & 0xAAAAAAAAu) + 2u * countbits(mask & 0xCCCCCCCCu) +
                                    4u * countbits(mask & 0xF0F0F0F0u) + 8u * countbits(mask & 0xFF00FF00u) +
                                    16u * countbits(mask & 0xFFFF0000u)) +
                              0.5 * count;
            const float bin = sky_half_pi / float(sky_bins);
            const float centroid = sum / count * bin;
            const float half_size = 0.5 * count * bin;
            arc.lo = centroid - half_size;
            arc.hi = centroid + half_size;
            if (arc.lo < 0.0) {
                arc.hi -= arc.lo;
                arc.lo = 0.0;
            }
            if (arc.hi > sky_half_pi) {
                arc.lo -= arc.hi - sky_half_pi;
                arc.hi = sky_half_pi;
            }
        }

        // Is what the point sees in this azimuth in each shadowed light's sun? Three points in the air just in front
        // of it: the middle of the hidden arc on the occluder, and the ground (or the occluder's foot) 20 and 50
        // degrees below the horizontal.
        const float3 horizontal = direction.x * f.axis_x.xyz + direction.y * f.axis_y.xyz;
        const float out = hit < 1e29 ? max(hit - 0.25, 0.25) : 0.0;
        const float to_occluder = hit < 1e29 ? out : max(hit_any - 0.25, 0.25);
        float3 q[3];
        q[0] = start + horizontal * to_occluder + up * (to_occluder * tan(min(0.5 * (arc.lo + arc.hi), 1.4)));
        const float depth_below = max(y - ground, 0.3);
        [unroll]
        for (uint g = 0; g < 2; ++g) {
            const float slope = g == 0 ? 0.36397 : 1.19175;  // tan 20, tan 50 degrees
            const float reach = depth_below / slope;         // where that ray meets the ground
            if (hit < 1e29 && out < reach) {
                q[1 + g] = start + horizontal * out - up * (out * slope);  // it meets the occluder first
            } else {
                q[1 + g] = start + horizontal * max(reach, 0.25) - up * max(depth_below - 0.3, 0.0);
            }
        }
        arc.lit_occluder = float2(1.0, 1.0);
        arc.lit_ground = float2(1.0, 1.0);
        [unroll]
        for (uint slot = 0; slot < 2; ++slot) {
            if (f.lights[slot].w > 0.0) {
                const float3 l = f.lights[slot].xyz;
                arc.lit_occluder[slot] = sky_sunlit(slot, q[0], l);
                arc.lit_ground[slot] = 0.5 * (sky_sunlit(slot, q[1], l) + sky_sunlit(slot, q[2], l));
            }
        }
        arcs[k] = arc;
    }

    // The ambient table with and without what is hidden, for this normal and a mid-rough reflection.
    const float3 mirror = reflect(-view, n);
    const float lean = (1.0 - sky_specular_alpha) * (sqrt(1.0 - sky_specular_alpha) + sky_specular_alpha);
    const float3 dominant = normalize(lerp(n, mirror, lean));
    SkyOcclusionTerms terms = sky_occlusion_terms(f, arcs, n, dominant, sky_specular_alpha);
    // A sun no shadow map follows (beyond the cascades' lights, or shadows off): the height map says whether the point
    // sees it, and that stands for everything it sees.
    for (uint i = 0; i < min(sky.sun_count, 4u); ++i) {
        if (sky_light_slot(terms, sky.sun_directions[i].xyz) == 2) {
            terms.sun_visible[i] = sky_map_visible(f, start, sky.sun_directions[i].xyz);
        }
    }
    float4 ratio;
    if (sky.mode == sky_constant) {
        // Uniform radiance, the ground's included: what the scene hides is dark.
        ratio = float4((1.0 - terms.blocked).xxx, 1.0 - terms.specular_blocked);
    } else {
        float3 open_diffuse;
        float3 open_specular;
        float3 diffuse;
        float3 specular;
        sky_ambient_occluded(sky, p, params.camera.xyz, dominant, sky_specular_alpha, terms, open_diffuse,
                             open_specular, diffuse, specular);
        const float3 floor_d = 1e-4 * dot(open_diffuse, float3(0.2126, 0.7152, 0.0722)).xxx + 1e-20;
        const float open_s = dot(open_specular, float3(0.2126, 0.7152, 0.0722));
        ratio.rgb = (diffuse + floor_d) / (open_diffuse + floor_d);
        ratio.a = (dot(specular, float3(0.2126, 0.7152, 0.0722)) + 1e-20) / (open_s + 1e-20);
    }
    params.ratios[id.xy] = lerp(float4(1.0, 1.0, 1.0, 1.0), clamp(ratio, 0.0, 2.0), fade);
}

// ---- the blur, over the cells -----------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void sky_blur(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.extent)) {
        return;
    }
    const int2 c = int2(id.xy);
    const float distance = params.near.Load(int3(c, 0));
    const int2 last = int2(params.extent) - 1;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 at = clamp(c + int2(x, y), int2(0, 0), last);
            const float d = params.near.Load(int3(at, 0));
            const float w = exp(-0.5 * float(x * x + y * y)) * saturate(1.0 - abs(d - distance) / (0.05 * distance));
            sum += w * params.raw.Load(int3(at, 0));
            weight += w;
        }
    }
    params.ratios[id.xy] = weight > 1e-6 ? sum / weight : params.raw.Load(int3(c, 0));
}
