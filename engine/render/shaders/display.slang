// The display transform (docs/adr/0037-exposure-and-agx.md): linear radiance in nits, times the camera's
// exposure, through AgX (base look), sRGB-encoded into scene_color. Both renderers go through it.

import veach.bindless;

struct Params {
    DescriptorHandle<Texture2D<float4>> radiance;  ///< rgba16f, nits
    DescriptorHandle<RWTexture2D<float4>> output;  ///< scene_color, rgba8 (sRGB-encoded values)
    uint2 size;
    float exposure;  ///< 1 / (1.2 * 2^ev100)
    uint tonemap;    ///< 1 = AgX; 0 = clamp (debug views that are already display values)
    uint2 input_size;  ///< radiance's size; a smaller one (a debug view at render resolution) is scaled, nearest
    uint2 pad;
};
[[vk::push_constant]] ConstantBuffer<Params> params;

// AgX, after Troy Sobotka's reference and Benjamin Wrensch's fit (as in Blender 4 and Godot): linear Rec.709 in,
// display-encoded Rec.709 out.
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

// The sigmoid, a 6th-order polynomial fit of the AgX base contrast curve, on the [0, 1] log-encoded value.
float3 agx_curve(float3 x) {
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agx(float3 linear) {
    // Row-vector convention of the reference matrices: color * M.
    float3 c = mul(linear, agx_inset);
    c = clamp(log2(max(c, 1e-10)), agx_min_ev, agx_max_ev);
    c = (c - agx_min_ev) / (agx_max_ev - agx_min_ev);
    c = agx_curve(c);
    c = mul(c, agx_outset);
    // The curve's output is display-encoded with a 2.2 power; decode to linear for the sRGB encoder below.
    return pow(max(c, 0.0), 2.2);
}

float linear_to_srgb(float c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void display(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.size)) {
        return;
    }
    const uint2 source = min(id.xy * params.input_size / params.size, params.input_size - 1);
    const float3 radiance = params.radiance.Load(int3(source, 0)).rgb;
    float3 c = radiance * params.exposure;
    if (params.tonemap == 1) {
        c = agx(c);
    }
    params.output[id.xy] = float4(linear_to_srgb(c.r), linear_to_srgb(c.g), linear_to_srgb(c.b), 1.0);
}
