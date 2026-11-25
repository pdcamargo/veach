// The real-time renderer's per-frame record (ADR 0040): matrices, the scene, and every persistent per-view
// target, read by all of its passes from one buffer (a pass's parameters carry only the buffer and an argument).
// Layout mirrors render::RealtimeFrame (realtime_renderer.cpp): float4x4 and float4 first, then 8-byte handles,
// then scalars in groups of four, so Metal and HLSL agree byte for byte.

import veach.bindless;
import veach.brdf;
import veach.material;
import veach.scene;

static const uint frame_history_valid = 1;  ///< RealtimeFrame::flags: last frame's targets belong to this view

// RealtimeFrame::view: what the renderer shows (render::RealtimeView).
static const uint view_final = 0;
static const uint view_noisy = 1;
static const uint view_motion = 2;
static const uint view_history = 3;

struct RealtimeFrame {
    float4x4 view_projection;             ///< jittered: what the visibility pass rasterizes with
    float4x4 inverse_view_projection;     ///< jittered: camera rays through each render pixel's sample
    float4x4 unjittered_view_projection;
    float4x4 previous_view_projection;    ///< last frame's, unjittered
    float4 camera_position;               ///< xyz; w = exposure (1 / (1.2 * 2^ev100))
    float4 previous_camera_position;      ///< xyz
    float4 sky_ambient;                   ///< xyz sky radiance (nits); w = 1 when the ambient stopgap is on
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
    DescriptorHandle<Texture2D<float4>> unused0;

    uint2 render_size;
    uint frame;  ///< seeds the random numbers
    uint flags;  ///< frame_history_valid
    uint light_count;
    uint emissive_count;
    float emissive_total_weight;
    uint scene_flags;
    uint emissive_samples;
    uint ao_rays;
    float ao_radius;
    uint debug_skip;
    uint view;  ///< view_*
    uint pad0;
    uint2 pad1;
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
