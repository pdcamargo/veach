// The real-time renderer's per-frame record (ADR 0040): matrices, the scene, and every persistent per-view
// target, read by all of its passes from one buffer (a pass's parameters carry only the buffer and an argument).
// Layout mirrors render::RealtimeFrame (realtime_renderer.cpp): float4x4 and float4 first, then 8-byte handles,
// then scalars in groups of four, so Metal and HLSL agree byte for byte.

import veach.atmosphere;
import veach.bindless;
import veach.brdf;
import veach.material;
import veach.scene;

/// Mirrors render::AliasEntry: Vose's alias table, one entry per light.
struct AliasEntry {
    float threshold;  ///< pick this entry if the second number is below it, else its alias
    uint alias;
    float pdf;        ///< this light's selection probability
    uint pad;
};

/// ReSTIR DI's reservoir as stored (veach/restir.slang unpacks it). float4 only, so Metal and HLSL lay it out
/// alike. Mirrors render::PackedReservoir: 48 bytes.
struct PackedReservoir {
    float4 position_light;  ///< xyz: a triangle sample's point; w: the light index, bit for bit
    float4 normal_w;        ///< xyz: a triangle sample's front normal; w: W
    float4 radiance_m;      ///< xyz: a triangle sample's front emitted radiance; w: M
};

static const uint frame_history_valid = 1;    ///< RealtimeFrame::flags: last frame's targets belong to this view
static const uint frame_reservoirs_valid = 2;
static const uint frame_denoiser_history_valid = 4;  ///< the denoiser ran last frame: its history is this view's  ///< RealtimeFrame::flags: last frame's reservoirs hold these lights' samples

// RealtimeFrame::gi: the indirect light (render::GlobalIllumination).
static const uint gi_off = 0;          ///< the ambient stopgap
static const uint gi_direct_only = 1;  ///< none

// RealtimeFrame::view: what the renderer shows (render::RealtimeView).
static const uint view_final = 0;
static const uint view_noisy = 1;
static const uint view_motion = 2;
static const uint view_history = 3;
static const uint view_accumulated = 4;

struct RealtimeFrame {
    float4x4 view_projection;             ///< jittered: what the visibility pass rasterizes with
    float4x4 inverse_view_projection;     ///< jittered: camera rays through each render pixel's sample
    float4x4 unjittered_view_projection;
    float4x4 previous_view_projection;    ///< last frame's, unjittered
    float4 camera_position;               ///< xyz; w = exposure (1 / (1.2 * 2^ev100))
    float4 previous_camera_position;      ///< xyz
    float4 reserved;
    float4 jitter;                        ///< xy: this frame's sample offset from the pixel center, render pixels

    DescriptorHandle<RaytracingAccelerationStructure> tlas;
    DescriptorHandle<StructuredBuffer<InstanceRecord>> instances;
    DescriptorHandle<StructuredBuffer<MaterialRecord>> materials;
    DescriptorHandle<StructuredBuffer<LightRecord>> lights;
    DescriptorHandle<StructuredBuffer<EmissiveTriangle>> emissive;
    DescriptorHandle<Texture2D<uint2>> visibility;

    // The surfaces (ADR 0040), this frame's (read and write) and last frame's (read).
    DescriptorHandle<Texture2D<float4>> position;   ///< world position, w = linear view depth (0: no geometry)
    DescriptorHandle<Texture2D<float4>> normal;     ///< shading normal, w = roughness
    DescriptorHandle<Texture2D<float4>> geometric;  ///< geometric normal facing the camera, w = metalness
    DescriptorHandle<Texture2D<float4>> diffuse;    ///< diffuse albedo, w = 1 if double sided
    DescriptorHandle<Texture2D<float4>> specular;   ///< specular F0
    DescriptorHandle<Texture2D<float4>> emission;   ///< emission seen directly, nits
    DescriptorHandle<Texture2D<float4>> motion;     ///< xy previous minus current pixel, unjittered; w = valid taps
    DescriptorHandle<RWTexture2D<float4>> position_out;
    DescriptorHandle<RWTexture2D<float4>> normal_out;
    DescriptorHandle<RWTexture2D<float4>> geometric_out;
    DescriptorHandle<RWTexture2D<float4>> diffuse_out;
    DescriptorHandle<RWTexture2D<float4>> specular_out;
    DescriptorHandle<RWTexture2D<float4>> emission_out;
    DescriptorHandle<RWTexture2D<float4>> motion_out;
    DescriptorHandle<Texture2D<float4>> previous_position;
    DescriptorHandle<Texture2D<float4>> previous_normal;
    DescriptorHandle<Texture2D<float4>> previous_geometric;
    DescriptorHandle<Texture2D<float4>> previous_diffuse;
    DescriptorHandle<Texture2D<float4>> previous_specular;
    DescriptorHandle<Texture2D<float4>> radiance;   ///< lit, exposed linear color (the upscaler's input)
    DescriptorHandle<RWTexture2D<float4>> radiance_out;
    // ReSTIR DI (ADR 0041, veach/restir.slang): one reservoir per render pixel, row-major.
    DescriptorHandle<StructuredBuffer<AliasEntry>> alias;                ///< light_count + emissive_count entries
    DescriptorHandle<RWStructuredBuffer<PackedReservoir>> temporal_out;        ///< initial + temporal reuse
    DescriptorHandle<StructuredBuffer<PackedReservoir>> temporal_reservoirs;   ///< the same, read by spatial reuse
    DescriptorHandle<RWStructuredBuffer<PackedReservoir>> final_out;           ///< after spatial reuse
    DescriptorHandle<StructuredBuffer<PackedReservoir>> final_reservoirs;      ///< the same, read by shading
    DescriptorHandle<StructuredBuffer<PackedReservoir>> previous_reservoirs;   ///< last frame's final reservoirs
    DescriptorHandle<Texture2D<float4>> accumulation;    ///< view_accumulated: the average so far
    DescriptorHandle<RWTexture2D<float4>> accumulation_out;
    // The denoiser (ADR 0042): demodulated diffuse and specular light, exposed.
    DescriptorHandle<Texture2D<float4>> noisy_diffuse;
    DescriptorHandle<RWTexture2D<float4>> noisy_diffuse_out;
    DescriptorHandle<Texture2D<float4>> noisy_specular;
    DescriptorHandle<RWTexture2D<float4>> noisy_specular_out;
    DescriptorHandle<Texture2D<float4>> previous_diffuse_history;   ///< rgb accumulated, a = frames in it
    DescriptorHandle<Texture2D<float4>> previous_specular_history;
    DescriptorHandle<RWTexture2D<float4>> diffuse_history_out;
    DescriptorHandle<RWTexture2D<float4>> specular_history_out;
    DescriptorHandle<Texture2D<float4>> diffuse_history;
    DescriptorHandle<Texture2D<float4>> specular_history;
    DescriptorHandle<Texture2D<float4>> previous_moments;  ///< luminance and its square: diffuse xy, specular zw
    DescriptorHandle<RWTexture2D<float4>> moments_out;
    DescriptorHandle<Texture2D<float4>> moments;
    DescriptorHandle<Texture2D<float4>> atrous_diffuse0;   ///< the spatial filter's ping-pong: rgb, a = variance
    DescriptorHandle<RWTexture2D<float4>> atrous_diffuse0_out;
    DescriptorHandle<Texture2D<float4>> atrous_diffuse1;
    DescriptorHandle<RWTexture2D<float4>> atrous_diffuse1_out;
    DescriptorHandle<Texture2D<float4>> atrous_specular0;
    DescriptorHandle<RWTexture2D<float4>> atrous_specular0_out;
    DescriptorHandle<Texture2D<float4>> atrous_specular1;
    DescriptorHandle<RWTexture2D<float4>> atrous_specular1_out;
    DescriptorHandle<Texture2D<float4>> denoised;          ///< remodulated, exposed: the upscaler's input
    DescriptorHandle<RWTexture2D<float4>> denoised_out;
    DescriptorHandle<RWStructuredBuffer<uint>> counters;   ///< [0] ReSTIR's shadow rays, [1] lit pixels, [2] directional lights' rays
    DescriptorHandle<StructuredBuffer<SkyFrame>> sky;      ///< the environment (ADR 0050)
    /// Indirect light on the diffuse lobe, written by the GI pass and added by shading: the cosine-weighted mean of
    /// the incoming radiance (nits, unexposed), which the diffuse albedo turns into outgoing radiance.
    DescriptorHandle<Texture2D<float4>> indirect;
    DescriptorHandle<RWTexture2D<float4>> indirect_out;

    uint2 render_size;
    uint frame;  ///< seeds the random numbers
    uint flags;  ///< frame_history_valid
    uint light_count;
    uint emissive_count;
    float emissive_total_weight;
    uint scene_flags;
    uint gi;  ///< gi_*
    uint ao_rays;
    float ao_radius;
    uint debug_skip;
    uint view;  ///< view_*
    uint light_candidates;
    uint spatial_samples;
    uint temporal_reuse;  ///< 1: reuse last frame's reservoirs
    uint accumulated;     ///< view_accumulated: frames in the average before this one
    uint denoise;         ///< 1: the denoiser runs (shade still writes the noisy image to radiance)
    uint2 pad2;
};

/// Fills veach_scene (veach/scene.slang) from the frame record.
void use_scene(RealtimeFrame f) {
    veach_scene.tlas = f.tlas;
    veach_scene.instances = f.instances;
    veach_scene.materials = f.materials;
    veach_scene.lights = f.lights;
    veach_scene.emissive = f.emissive;
    veach_scene.light_count = f.light_count;
    veach_scene.emissive_count = f.emissive_count;
    veach_scene.emissive_total_weight = f.emissive_total_weight;
    veach_scene.debug_skip = f.debug_skip;
    veach_scene.flags = f.scene_flags;
}

/// Pixel coordinates (0..size, y down) of world point p under `view_projection`; z = clip w (behind: <= 0).
float3 to_pixel(float4x4 view_projection, float3 p, uint2 size) {
    const float4 clip = mul(view_projection, float4(p, 1.0));
    const float2 ndc = clip.xy / clip.w;
    return float3((ndc.x * 0.5 + 0.5) * float(size.x), (0.5 - ndc.y * 0.5) * float(size.y), clip.w);
}

/// A pixel's surface, as the surface pass wrote it.
struct GSurface {
    bool valid;  ///< false: no geometry (sky)
    float3 position;
    float depth;  ///< linear view depth
    float3 normal;
    float roughness;
    float3 geometric;
    float metallic;
    float3 diffuse;
    float3 f0;
    float3 emission;
};

GSurface load_surface(RealtimeFrame f, int2 pixel) {
    GSurface s;
    const float4 p = f.position.Load(int3(pixel, 0));
    s.valid = p.w > 0.0;
    s.position = p.xyz;
    s.depth = p.w;
    const float4 n = f.normal.Load(int3(pixel, 0));
    s.normal = n.xyz;
    s.roughness = n.w;
    const float4 g = f.geometric.Load(int3(pixel, 0));
    s.geometric = g.xyz;
    s.metallic = g.w;
    s.diffuse = f.diffuse.Load(int3(pixel, 0)).xyz;
    s.f0 = f.specular.Load(int3(pixel, 0)).xyz;
    s.emission = f.emission.Load(int3(pixel, 0)).xyz;
    return s;
}

/// A pixel's surface without its emission: what lighting another pixel's domain needs.
GSurface load_lit_surface(RealtimeFrame f, int2 pixel) {
    GSurface s;
    const float4 p = f.position.Load(int3(pixel, 0));
    s.valid = p.w > 0.0;
    s.position = p.xyz;
    s.depth = p.w;
    const float4 n = f.normal.Load(int3(pixel, 0));
    s.normal = n.xyz;
    s.roughness = n.w;
    const float4 g = f.geometric.Load(int3(pixel, 0));
    s.geometric = g.xyz;
    s.metallic = g.w;
    s.diffuse = f.diffuse.Load(int3(pixel, 0)).xyz;
    s.f0 = f.specular.Load(int3(pixel, 0)).xyz;
    s.emission = float3(0.0, 0.0, 0.0);
    return s;
}

/// Last frame's surface at a pixel (no emission: nothing reads it).
GSurface load_previous_surface(RealtimeFrame f, int2 pixel) {
    GSurface s;
    const float4 p = f.previous_position.Load(int3(pixel, 0));
    s.valid = p.w > 0.0;
    s.position = p.xyz;
    s.depth = p.w;
    const float4 n = f.previous_normal.Load(int3(pixel, 0));
    s.normal = n.xyz;
    s.roughness = n.w;
    const float4 g = f.previous_geometric.Load(int3(pixel, 0));
    s.geometric = g.xyz;
    s.metallic = g.w;
    s.diffuse = f.previous_diffuse.Load(int3(pixel, 0)).xyz;
    s.f0 = f.previous_specular.Load(int3(pixel, 0)).xyz;
    s.emission = float3(0.0, 0.0, 0.0);
    return s;
}

/// What the denoiser divides light by before filtering and multiplies back after (ADR 0042): the diffuse albedo and
/// the specular lobe's albedo, floored so dark surfaces keep their signal.
struct Demodulation {
    float3 diffuse;
    float3 specular;
};

Demodulation demodulation_of(GSurface s, float3 camera) {
    Demodulation d;
    const float n_dot_v = saturate(dot(s.normal, normalize(camera - s.position)));
    d.diffuse = max(s.diffuse, 0.001);
    d.specular = max(specular_albedo(s.f0, s.roughness, n_dot_v), 0.001);
    return d;
}

/// The BRDF the path tracer would use at this surface (veach/brdf.slang's brdf_of, from stored terms).
Brdf brdf_of_surface(GSurface s, float n_dot_v) {
    Brdf b;
    b.diffuse = s.diffuse;
    b.f0 = s.f0;
    const float roughness = max(s.roughness, 0.02);
    b.alpha = roughness * roughness;
    const float spec = luminance(b.f0 + (1.0 - b.f0) * pow(1.0 - n_dot_v, 5.0));
    const float diff = luminance(b.diffuse);
    b.specular_probability = spec + diff > 0.0 ? clamp(spec / (spec + diff), 0.1, 0.9) : 0.5;
    return b;
}
