// The space backdrop's bake (ADR 0090, docs/specs/space-backdrop/spec.md): the galactic band (disk light, bulge, star clouds, dust lanes) and the nebulae, once, into a lat-long
// rgba16f texture in galactic coordinates. rgb is radiance (nits); a is the star density the per-pixel stars use
// there (veach.backdrop). Run when the backdrop's baked fields change (render::Backdrop::bakes_like). Its parameter
// layout matches render::BackdropBakeParams (backdrop.cpp).

import veach.backdrop;
import veach.bindless;

/// Mirrors render::BackdropNebulaRecord.
struct NebulaRecord {
    float4 direction;  ///< xyz = center (galactic), w = angular radius (rad)
    float4 color;      ///< xyz = rim / reflection color, w = brightness (dark: optical depth)
    float4 core;       ///< xyz = core color, w = the core's share
    float4 shape;      ///< x = elongation, y = position angle (rad)
    uint4 info;        ///< x = kind (0 emission, 1 reflection, 2 dark), y = seed
};

/// Mirrors render::BackdropBakeRecord.
struct BakeRecord {
    float4 band;  ///< x = brightness (nits), y = latitude (rad), z = half-width (rad), w = bulge
    float4 more;  ///< x = anticenter, y = dust
    uint4 info;   ///< x = seed, y = nebula count
    NebulaRecord nebulae[8];
};

struct BakeParams {
    DescriptorHandle<StructuredBuffer<BakeRecord>> record;
    DescriptorHandle<RWTexture2D<float4>> output;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<BakeParams> params;

static const float pi = 3.14159265358979;

/// The band's disk light, bulge and dust at galactic direction `g`: radiance over the band's brightness (rgb) and the
/// star density (a).
float4 band(BakeRecord r, float3 g, uint2 texel) {
    const float longitude = atan2(g.z, g.x);
    const float latitude = asin(clamp(g.y, -1.0, 1.0));
    const uint seed = r.info.x;
    const float width = r.band.z;
    // The plane wanders a little, as the real band's center line does.
    float3 flow;
    const float wobble = backdrop_fbm(g * 2.0, seed + 401u, 3) * 0.35 * width;
    const float b = latitude - r.band.y - wobble;
    // Wider toward the center (where the disk is nearer and the bulge thickens it).
    const float toward_center = 0.5 + 0.5 * cos(longitude);
    const float w = width * (0.75 + 0.6 * toward_center * toward_center);
    const float along = lerp(r.more.x, 1.0, pow(toward_center, 1.6));
    const float core = exp(-0.5 * (b / (0.55 * w)) * (b / (0.55 * w)));
    const float wings = exp(-abs(b) / (1.4 * w));
    const float halo = 0.06 * exp(-abs(b) / 0.5);  // the thick disk and halo: a faint glow far from the plane
    const float profile = 0.6 * core + 0.4 * wings;
    if (profile < 0.002) {
        // Far from the plane only the halo's glow is left (the clouds, the bulge and the dust are below 0.5 % here):
        // most of the sky skips the noise.
        return float4(along * halo * float3(1.0, 0.93, 0.84), 0.3);
    }

    // Star clouds: the disk's light gathered into clumps, finer than the band is wide, mottled by smaller clouds and
    // speckled per texel by the brightest unresolved stars, so the band reads as made of stars rather than smoke.
    const float clouds = backdrop_warped(g * 5.5, seed + 11u, 7, 0.6, flow);
    const float clump = saturate(0.5 + 0.9 * clouds);
    const float cloud_light = lerp(0.15, 2.0, clump * clump);
    const float fine = backdrop_fbm(g * 55.0, seed + 211u, 5);
    const float mottle = lerp(0.45, 1.5, saturate(0.5 + 1.1 * fine));
    const float speck = pow(backdrop_unit(backdrop_hash(uint3(texel, seed + 977u))), 10.0) * 5.0;
    float disk = along * (profile * cloud_light * (mottle + speck * clump) + halo);

    // Young blue clusters in the clouds, older yellow light between them.
    const float young = saturate(backdrop_fbm(g * 11.0 + 3.0, seed + 307u, 4) * 1.6);
    float3 color = lerp(float3(1.0, 0.90, 0.76), float3(0.80, 0.90, 1.12), young * clump);

    // The bulge: a boxy, warm glow around the center, peanut-shaped across the plane.
    const float lx = longitude / 0.36;
    const float by = b / 0.22;
    const float bulge = 2.5 * r.band.w * exp(-pow(lx * lx * lx * lx + by * by * by * by, 0.35) * 2.2) *
                        lerp(0.75, 1.2, saturate(0.5 + 0.8 * fine));
    float3 light = disk * color + bulge * float3(1.0, 0.80, 0.56);

    // Dust lanes: a thinner sheet than the stars, broken into filaments, darkest and widest toward the center (the
    // Great Rift). Reddens what it dims (more extinction in blue).
    float3 dust_flow;
    const float lane = backdrop_warped(g * 9.0 + 7.0, seed + 503u, 7, 1.3, dust_flow);
    const float filaments = pow(saturate(1.0 - abs(backdrop_fbm(g * 16.0 + dust_flow * 0.8, seed + 601u, 5))), 3.0);
    const float sheet = exp(-0.5 * (b / (0.33 * w)) * (b / (0.33 * w))) + 0.3 * exp(-abs(b) / (0.9 * w));
    const float tau = r.more.y * sheet * (0.35 + 0.65 * toward_center) *
                      (2.4 * saturate(0.25 + 1.1 * lane) * saturate(0.25 + 1.1 * lane) + 1.2 * filaments);
    const float3 transmittance = exp(-tau * float3(0.75, 1.0, 1.4));
    light *= transmittance;

    const float stars = (0.3 + 3.2 * along * profile * clump + 0.6 * bulge) * transmittance.g;
    return float4(light, stars);
}

/// A nebula's tangent-plane coordinates around its center, in radii, turned and stretched.
float2 nebula_plane(NebulaRecord n, float3 g) {
    const float3 c = n.direction.xyz;
    const float3 helper = abs(c.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 e1 = normalize(cross(helper, c));
    const float3 e2 = cross(c, e1);
    const float2 p = float2(dot(g, e1), dot(g, e2)) / max(dot(g, c), 1e-3) / n.direction.w;
    const float ca = cos(n.shape.y);
    const float sa = sin(n.shape.y);
    const float2 q = float2(ca * p.x + sa * p.y, -sa * p.x + ca * p.y);
    return float2(q.x / n.shape.x, q.y);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void backdrop_bake(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    const BakeRecord r = params.record[0];
    const float3 g = backdrop_direction((float2(id.xy) + 0.5) / float2(params.size));
    float4 result = band(r, g, id.xy);
    float3 light = result.rgb;
    float stars = result.a;

    // Emission and reflection nebulae add light; dark ones then dim everything behind them.
    for (uint pass = 0; pass < 2; ++pass) {
        for (uint i = 0; i < min(r.info.y, 8u); ++i) {
            const NebulaRecord n = r.nebulae[i];
            const bool dark = n.info.x == 2u;
            if (dark != (pass == 1u) || dot(g, n.direction.xyz) < cos(min(n.direction.w * 2.2 * n.shape.x, 1.5))) {
                continue;
            }
            const float2 q = nebula_plane(n, g);
            const float3 p = g * (2.4 / n.direction.w);
            float3 flow;
            const float d = backdrop_warped(p, n.info.y, 7, 0.9, flow);
            // An irregular edge: the radius itself pushed around by the flow.
            const float radius = length(q) * (1.0 + 0.55 * flow.x) + 0.25 * flow.y;
            const float envelope = exp(-2.0 * radius * radius) * saturate(1.8 - radius);
            if (envelope <= 1e-4) {
                continue;
            }
            if (n.info.x == 0u) {
                // H II: a glowing body with bright ionization fronts along its contours (where the gas meets the
                // hot stars' light) and thin H-alpha sheets, around an O III core lit by the stars at its center, cut
                // by dark dust lanes and pillars.
                const float body = saturate(0.3 + 1.0 * d);
                const float front = exp(-((d - 0.12) * (d - 0.12)) / 0.006);
                const float sheets = 1.0 - abs(backdrop_fbm(p * 1.3 + flow * 1.5, n.info.y + 41u, 6));
                const float ridge = pow(saturate(sheets), 9.0);
                const float center = exp(-6.0 * radius * radius);
                const float density = envelope * (body * body * body * 1.1 + front * 1.4 * saturate(0.4 + envelope) +
                                                  ridge * 1.6 * saturate(0.25 + d)) +
                                      2.0 * center * body;
                const float core = n.core.w * saturate(exp(-3.0 * radius * radius) * (0.4 + 1.2 * body));
                const float3 color = lerp(n.color.xyz, n.core.xyz * 1.7, 0.8 * core);
                const float lanes = saturate(backdrop_fbm(p * 2.8 + flow * 0.5 + 13.0, n.info.y + 97u, 6) * 2.0 - 0.15);
                light += n.color.w * density * color * exp(-4.0 * lanes * lanes * envelope);
            } else if (n.info.x == 1u) {
                // Reflection: soft, wispy, blue, brightest around its (unseen) stars.
                const float wisps = saturate(0.4 + 0.9 * d);
                const float density = envelope * (wisps * wisps * 1.2 + 0.6 * exp(-6.0 * radius * radius));
                light += n.color.w * density * n.color.xyz;
            } else {
                const float clump = saturate(0.35 + 1.0 * d);
                const float tau = n.color.w * envelope * clump * clump * 1.6;
                light *= exp(-tau * float3(0.8, 1.0, 1.25));
                stars *= exp(-tau);
            }
        }
    }
    light *= r.band.x;
    params.output[id.xy] = float4(min(light, float3(65504.0, 65504.0, 65504.0)), stars);
}
