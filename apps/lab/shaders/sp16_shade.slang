// Spike SP-16 (VCH-092), THROWAWAY: path A's material pass. Each pixel of the visibility buffer names a triangle
// ({instance, part << 20 | primitive}); the surface is rebuilt from it as ADR 0040 does (barycentrics from the pixel's
// ray, UV gradients from the neighbors' rays) and lit by the same shade_point as the forward path. `display` turns
// either path's radiance into scene_color (AgX, as display.slang). `quad_stats` measures how many 2x2 quads a
// forward pass would shade for the visible surface. Layout matches ShadeParams in sp16_spike.cpp.

import veach.bindless;
import sp16.common;

struct ShadeParams {
    Scene scene;
    DescriptorHandle<Texture2D<uint2>> visibility;
    DescriptorHandle<RWTexture2D<float4>> radiance;  ///< shade: written; display: read (as storage)
    DescriptorHandle<RWTexture2D<float4>> output;    ///< display: scene_color
    DescriptorHandle<RWStructuredBuffer<uint>> counters;  ///< quad_stats: [2] covered pixels, [3] (quad, triangle) pairs
    uint2 size;
    uint flags;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<ShadeParams> params;

float3 camera_ray(FrameData f, float2 pixel) {
    const float2 ndc = float2(pixel.x / float(params.size.x) * 2.0 - 1.0, 1.0 - pixel.y / float(params.size.y) * 2.0);
    const float4 near = mul_columns(f.inverse_view_projection, float4(ndc, 1.0, 1.0));
    const float4 mid = mul_columns(f.inverse_view_projection, float4(ndc, 0.5, 1.0));
    return normalize(mid.xyz / mid.w - near.xyz / near.w);
}

float2 plane_barycentrics(float3 origin, float3 direction, float3 p0, float3 p1, float3 p2) {
    const float3 e1 = p1 - p0;
    const float3 e2 = p2 - p0;
    const float3 pv = cross(direction, e2);
    const float det = dot(e1, pv);
    const float inv = abs(det) > 1e-20 ? 1.0 / det : 0.0;
    const float3 tv = origin - p0;
    const float3 qv = cross(tv, e1);
    return float2(dot(tv, pv) * inv, dot(direction, qv) * inv);
}

float2 inside_triangle(float2 b) {
    float3 w = max(float3(1.0 - b.x - b.y, b.x, b.y), float3(0.0, 0.0, 0.0));
    w /= max(w.x + w.y + w.z, 1e-20);
    return w.yz;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void shade(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    const uint2 vis = params.visibility.Load(int3(id.xy, 0));
    if (vis.x == no_geometry) {
        const FrameData f = params.scene.frame[0];
        params.radiance[id.xy] = float4(sky_radiance(f, camera_ray(f, float2(id.xy) + 0.5)) * f.camera.w, 1.0);
        return;
    }
    const Scene s = params.scene;
    const FrameData f = s.frame[0];
    const Instance inst = s.instances[vis.x];
    const Part part = s.parts[vis.y >> 20];
    const Material m = s.materials[part.material];
    const uint first = part.first_index + (vis.y & 0xFFFFFu) * 3;
    const Vertex v0 = s.vertices[s.indices[first]];
    const Vertex v1 = s.vertices[s.indices[first + 1]];
    const Vertex v2 = s.vertices[s.indices[first + 2]];
    const float3 p0 = to_world(inst, v0.position_u.xyz, 1.0);
    const float3 p1 = to_world(inst, v1.position_u.xyz, 1.0);
    const float3 p2 = to_world(inst, v2.position_u.xyz, 1.0);
    const float3 origin = f.camera.xyz;
    const float2 pixel = float2(id.xy) + 0.5;
    const float2 b = inside_triangle(plane_barycentrics(origin, camera_ray(f, pixel), p0, p1, p2));
    const float2 bx = plane_barycentrics(origin, camera_ray(f, pixel + float2(1.0, 0.0)), p0, p1, p2);
    const float2 by = plane_barycentrics(origin, camera_ray(f, pixel + float2(0.0, 1.0)), p0, p1, p2);
    const float3 w = float3(1.0 - b.x - b.y, b.x, b.y);

    ShadePoint p;
    p.position = p0 * w.x + p1 * w.y + p2 * w.z;
    p.normal = normalize(to_world(inst, v0.normal_v.xyz * w.x + v1.normal_v.xyz * w.y + v2.normal_v.xyz * w.z, 0.0));
    p.geometric_normal = normalize(cross(p1 - p0, p2 - p0));
    const float2 t0 = material_uv(m, float2(v0.position_u.w, v0.normal_v.w), p0);
    const float2 t1 = material_uv(m, float2(v1.position_u.w, v1.normal_v.w), p1);
    const float2 t2 = material_uv(m, float2(v2.position_u.w, v2.normal_v.w), p2);
    p.uv = t0 * w.x + t1 * w.y + t2 * w.z;
    p.uv_dx = t0 * (1.0 - bx.x - bx.y) + t1 * bx.x + t2 * bx.y - p.uv;
    p.uv_dy = t0 * (1.0 - by.x - by.y) + t1 * by.x + t2 * by.y - p.uv;
    const float2 u0 = float2(v0.position_u.w, v0.normal_v.w);
    const float2 u1 = float2(v1.position_u.w, v1.normal_v.w);
    const float2 u2 = float2(v2.position_u.w, v2.normal_v.w);
    p.vertex_uv = u0 * w.x + u1 * w.y + u2 * w.z;
    params.radiance[id.xy] = float4(shade_point(s, m, p), 1.0);
}

// AgX (display.slang's, Sobotka's reference with Wrensch's fit).
static const float3x3 agx_inset = float3x3(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
static const float3x3 agx_outset = float3x3(
    1.19687900512017, -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
static const float agx_min_ev = -12.47393;
static const float agx_max_ev = 4.026069;

float3 agx(float3 linear) {
    float3 c = mul(linear, agx_inset);
    c = clamp(log2(max(c, 1e-10)), agx_min_ev, agx_max_ev);
    c = (c - agx_min_ev) / (agx_max_ev - agx_min_ev);
    const float3 x2 = c * c;
    const float3 x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    c = mul(c, agx_outset);
    return pow(max(c, 0.0), 2.2);
}

float linear_to_srgb(float c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void display(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    // flags > 1: a supersampled reference, flags x flags radiance samples per output pixel, box-filtered.
    const uint n = max(params.flags, 1u);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint y = 0; y < n; ++y) {
        for (uint x = 0; x < n; ++x) {
            sum += params.radiance[id.xy * n + uint2(x, y)].rgb;
        }
    }
    const float3 c = agx(sum / float(n * n));
    params.output[id.xy] = float4(linear_to_srgb(c.r), linear_to_srgb(c.g), linear_to_srgb(c.b), 1.0);
}

/// Per 2x2 quad: the pixels with geometry, and the distinct triangles among them. A forward pass that shades only
/// the visible surface (after a depth prepass) runs 4 lanes per (quad, triangle) pair.
[shader("compute")]
[numthreads(8, 8, 1)]
void quad_stats(uint3 id: SV_DispatchThreadID) {
    const uint2 q = id.xy * 2;
    if (any(q >= params.size)) {
        return;
    }
    uint2 ids[4];
    uint covered = 0;
    uint distinct = 0;
    for (uint i = 0; i < 4; ++i) {
        const uint2 p = min(q + uint2(i & 1, i >> 1), params.size - 1);
        ids[i] = params.visibility.Load(int3(p, 0));
        if (ids[i].x == no_geometry) {
            continue;
        }
        ++covered;
        bool seen = false;
        for (uint j = 0; j < i; ++j) {
            seen = seen || all(ids[j] == ids[i]);
        }
        distinct += seen ? 0 : 1;
    }
    InterlockedAdd(params.counters[2], covered);
    InterlockedAdd(params.counters[3], distinct);
}
