#include <metal_stdlib>
#include <metal_math>
#include <metal_texture>
using namespace metal;
struct RayDesc
{
    float3 Origin;
    float TMin;
    float3 Direction;
    float TMax;
};

            raytracing::intersection_params _slang_ray_flags_to_intersection_params(uint flags){
                raytracing::intersection_params params;
                if (flags & 0x04) /* RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH */
                    params.accept_any_intersection(true);
                if (flags & 0x01) /* RAY_FLAG_FORCE_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::opaque);
                if (flags & 0x02) /* RAY_FLAG_FORCE_NON_OPAQUE */
                    params.force_opacity(raytracing::forced_opacity::non_opaque);
                if (flags & 0x10) /* RAY_FLAG_CULL_BACK_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::back);
                if (flags & 0x20) /* RAY_FLAG_CULL_FRONT_FACING_TRIANGLES */
                    params.set_triangle_cull_mode(raytracing::triangle_cull_mode::front);
                if (flags & 0x40) /* RAY_FLAG_CULL_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::opaque);
                if (flags & 0x80) /* RAY_FLAG_CULL_NON_OPAQUE */
                    params.set_opacity_cull_mode(raytracing::opacity_cull_mode::non_opaque);
                if (flags & 0x100) /* RAY_FLAG_SKIP_TRIANGLES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::triangle);
                if (flags & 0x200) /* RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES */
                    params.set_geometry_cull_mode(raytracing::geometry_cull_mode::bounding_box);
                return params;
            }
        

                uint _slang_candidate_intersection_type_to_uint(raytracing::intersection_type type){
                    switch(type){
                        case raytracing::intersection_type::triangle:
                            return 0; /* CANDIDATE_NON_OPAQUE_TRIANGLE */
                        case raytracing::intersection_type::bounding_box:
                            return 1; /* CANDIDATE_PROCEDURAL_PRIMITIVE */
                        default:
                            return 0xFFFFFFFF; // Invalid value to indicate error case, should never be returned by the API
                    }
                }
            

                uint _slang_committed_intersection_type_to_uint(raytracing::intersection_type type){
                    switch(type){
                        case raytracing::intersection_type::none:
                            return 0; /* COMMITTED_NOTHING */
                        case raytracing::intersection_type::triangle:
                            return 1; /* COMMITTED_TRIANGLE_HIT */
                        case raytracing::intersection_type::bounding_box:
                            return 2; /* COMMITTED_PROCEDURAL_PRIMITIVE_HIT */
                        default:
                            return 0xFFFFFFFF; // Invalid value to indicate error case, should never be returned by the API
                    }
                }
            

#line 17 "engine/render/shaders/veach/material.slang"
struct SurfaceInput_0
{
    float3 position_0;
    float3 geometric_normal_0;
    float3 normal_0;
    float4 tangent_0;
    float2 uv_0;
    float2 uv_dx_0;
    float2 uv_dy_0;
    float3 view_0;
};


#line 39
struct MaterialRecord_0
{
    uint kind_0;
    uint flags_0;
    float alpha_cutoff_0;
    float normal_scale_0;
    float4 base_color_0;
    float4 emission_metallic_0;
    float roughness_0;
    float emissive_weight_0;
    sampler sampler_0;
    texture2d<float, access::sample> base_color_texture_0;
    texture2d<float, access::sample> metallic_roughness_texture_0;
    texture2d<float, access::sample> normal_texture_0;
    texture2d<float, access::sample> emissive_texture_0;
    float4 custom0_0;
    float4 custom1_0;
};


#line 57
struct MaterialContext_0
{
    SurfaceInput_0 surface_0;
    MaterialRecord_0 record_0;
};


#line 74
float4 sampleTexture_0(texture2d<float, access::sample> texture_0, sampler sampler_1, float2 uv_1)
{

#line 75
    texture2d<float, access::sample> _S1 = texture_0;

#line 75
    return ((_S1).sample((sampler_1), (uv_1), level((0.0f))));
}


float4 sampleTexture_1(texture2d<float, access::sample> texture_1, sampler sampler_2, const SurfaceInput_0 thread* s_0)
{

#line 80
    float2 _S2 = s_0->uv_dx_0;

#line 80
    float2 _S3 = float2(0.0f) ;

#line 80
    bool _S4;
    if(all((s_0->uv_dx_0) == _S3))
    {

#line 81
        _S4 = all((s_0->uv_dy_0) == _S3);

#line 81
    }
    else
    {

#line 81
        _S4 = false;

#line 81
    }

#line 81
    if(_S4)
    {

#line 82
        float4 _S5 = sampleTexture_0(texture_1, sampler_2, s_0->uv_0);

#line 82
        return _S5;
    }
    texture2d<float, access::sample> _S6 = texture_1;

#line 84
    return ((_S6).sample((sampler_2), (s_0->uv_0), gradient2d((_S2), (s_0->uv_dy_0))));
}


#line 124
float3 standardEmission_0(const MaterialContext_0 thread* c_0)
{

#line 125
    float3 e_0 = (&c_0->record_0)->emission_metallic_0.xyz;

#line 125
    float3 e_1;
    if((((&c_0->record_0)->flags_0) & 8U) != 0U)
    {

#line 126
        float4 _S7 = sampleTexture_1((&c_0->record_0)->emissive_texture_0, (&c_0->record_0)->sampler_0, &c_0->surface_0);

#line 126
        e_1 = e_0 * _S7.xyz;

#line 126
    }
    else
    {

#line 126
        e_1 = e_0;

#line 126
    }


    return e_1;
}


#line 140
float3 IMaterial_evalEmissionx24defaultImpl_0(const MaterialContext_0 thread* c_1)
{

#line 140
    float3 _S8 = standardEmission_0(c_1);

#line 140
    return _S8;
}


#line 115
float2 standardMetallicRoughness_0(const MaterialContext_0 thread* c_2)
{

#line 116
    float2 mr_0 = float2((&c_2->record_0)->emission_metallic_0.w, (&c_2->record_0)->roughness_0);

#line 116
    float2 mr_1;
    if((((&c_2->record_0)->flags_0) & 2U) != 0U)
    {

#line 117
        float4 _S9 = sampleTexture_1((&c_2->record_0)->metallic_roughness_texture_0, (&c_2->record_0)->sampler_0, &c_2->surface_0);

#line 117
        mr_1 = mr_0 * float2(_S9.z, _S9.y);

#line 117
    }
    else
    {

#line 117
        mr_1 = mr_0;

#line 117
    }



    return mr_1;
}


#line 138
float2 IMaterial_evalMetallicRoughnessx24defaultImpl_0(const MaterialContext_0 thread* c_3)
{

#line 138
    float2 _S10 = standardMetallicRoughness_0(c_3);

#line 138
    return _S10;
}


#line 97
float3 standardNormal_0(const MaterialContext_0 thread* c_4)
{

#line 98
    float3 n_0 = normalize((&c_4->surface_0)->normal_0);
    if((((&c_4->record_0)->flags_0) & 4U) == 0U)
    {

#line 100
        return n_0;
    }

#line 100
    float4 _S11 = (&c_4->surface_0)->tangent_0;

    float3 _S12 = (&c_4->surface_0)->tangent_0.xyz;

#line 102
    float3 t_0 = _S12 - n_0 * float3(dot(n_0, _S12)) ;
    if((dot(t_0, t_0)) < 9.999999960041972e-13f)
    {

#line 104
        return n_0;
    }
    float3 t_1 = normalize(t_0);
    float3 _S13 = cross(n_0, t_1);

#line 107
    float _S14;

#line 107
    if((_S11.w) < 0.0f)
    {

#line 107
        _S14 = -1.0f;

#line 107
    }
    else
    {

#line 107
        _S14 = 1.0f;

#line 107
    }

#line 107
    float3 b_0 = _S13 * float3(_S14) ;

#line 107
    float4 _S15 = sampleTexture_1((&c_4->record_0)->normal_texture_0, (&c_4->record_0)->sampler_0, &c_4->surface_0);
    float3 _S16 = _S15.xyz * float3(2.0f)  - float3(1.0f) ;

#line 108
    thread float3 m_0 = _S16;
    m_0.xy = _S16.xy * float2((&c_4->record_0)->normal_scale_0) ;
    float3 shading_0 = normalize(float3(m_0.x)  * t_1 + float3(m_0.y)  * b_0 + float3(m_0.z)  * n_0);

#line 110
    float3 _S17;

    if((dot(shading_0, (&c_4->surface_0)->geometric_normal_0)) > 0.0f)
    {

#line 112
        _S17 = shading_0;

#line 112
    }
    else
    {

#line 112
        _S17 = n_0;

#line 112
    }

#line 112
    return _S17;
}


#line 136
float3 IMaterial_evalNormalx24defaultImpl_0(const MaterialContext_0 thread* c_5)
{

#line 136
    float3 _S18 = standardNormal_0(c_5);

#line 136
    return _S18;
}


#line 138
float2 IMaterial_evalMetallicRoughnessx24defaultImpl_1(const MaterialContext_0 thread* c_6)
{

#line 138
    float2 _S19 = standardMetallicRoughness_0(c_6);

#line 138
    return _S19;
}


#line 136
float3 IMaterial_evalNormalx24defaultImpl_1(const MaterialContext_0 thread* c_7)
{

#line 136
    float3 _S20 = standardNormal_0(c_7);

#line 136
    return _S20;
}


#line 44 "engine/render/shaders/veach/scene.slang"
struct LightRecord_natural_0
{
    packed_float4 position_1;
    packed_float4 direction_0;
    packed_float4 radiance_0;
    float cos_inner_0;
    float cos_outer_0;
    uint kind_1;
    float size_0;
};



struct EmissiveTriangle_natural_0
{
    uint instance_0;
    uint primitive_0;
    float cdf_0;
    float pad_0;
    packed_float4 p0_0;
    packed_float4 p1_0;
    packed_float4 p2_0;
};


#line 63 "engine/render/shaders/veach/material.slang"
struct SurfaceSample_0
{
    float3 base_color_1;
    float alpha_0;
    float3 normal_1;
    float metallic_0;
    float roughness_1;
    float3 emission_0;
};


#line 89
float4 standardBaseColor_0(const MaterialContext_0 thread* c_8)
{

#line 89
    float4 _S21 = (&c_8->record_0)->base_color_0;

#line 89
    float4 color_0;

    if((((&c_8->record_0)->flags_0) & 1U) != 0U)
    {

#line 91
        float4 _S22 = sampleTexture_1((&c_8->record_0)->base_color_texture_0, (&c_8->record_0)->sampler_0, &c_8->surface_0);

#line 91
        color_0 = _S21 * _S22;

#line 91
    }
    else
    {

#line 91
        color_0 = _S21;

#line 91
    }


    return color_0;
}


#line 134
float4 IMaterial_evalBaseColorx24defaultImpl_0(const MaterialContext_0 thread* c_9)
{

#line 134
    float4 _S23 = standardBaseColor_0(c_9);

#line 134
    return _S23;
}


#line 146
SurfaceSample_0 evaluate_0(const MaterialContext_0 thread* c_10)
{

#line 146
    float4 _S24 = IMaterial_evalBaseColorx24defaultImpl_0(c_10);
    thread SurfaceSample_0 s_1;

    (&s_1)->base_color_1 = _S24.xyz;
    (&s_1)->alpha_0 = _S24.w;

#line 150
    float3 _S25 = IMaterial_evalNormalx24defaultImpl_0(c_10);
    (&s_1)->normal_1 = _S25;

#line 151
    float2 _S26 = IMaterial_evalMetallicRoughnessx24defaultImpl_0(c_10);
    float2 mr_2 = saturate(_S26);
    (&s_1)->metallic_0 = mr_2.x;
    (&s_1)->roughness_1 = mr_2.y;

#line 154
    float3 _S27 = IMaterial_evalEmissionx24defaultImpl_0(c_10);
    (&s_1)->emission_0 = _S27;
    return s_1;
}


#line 134
float4 IMaterial_evalBaseColorx24defaultImpl_1(const MaterialContext_0 thread* c_11)
{

#line 134
    float4 _S28 = standardBaseColor_0(c_11);

#line 134
    return _S28;
}


#line 6 "apps/lab/shaders/glow_material.slang"
float3 GlowMaterial_evalEmission_0(const MaterialContext_0 thread* c_12)
{

#line 7
    float stripes_0 = step(0.5f, fract(((&c_12->surface_0)->uv_0.x + (&c_12->surface_0)->uv_0.y) * 3.0f));

#line 7
    float3 _S29 = standardEmission_0(c_12);
    return _S29 * float3(stripes_0) ;
}


#line 146 "engine/render/shaders/veach/material.slang"
SurfaceSample_0 evaluate_1(const MaterialContext_0 thread* c_13)
{

#line 146
    float4 _S30 = IMaterial_evalBaseColorx24defaultImpl_1(c_13);
    thread SurfaceSample_0 s_2;

    (&s_2)->base_color_1 = _S30.xyz;
    (&s_2)->alpha_0 = _S30.w;

#line 150
    float3 _S31 = IMaterial_evalNormalx24defaultImpl_1(c_13);
    (&s_2)->normal_1 = _S31;

#line 151
    float2 _S32 = IMaterial_evalMetallicRoughnessx24defaultImpl_1(c_13);
    float2 mr_3 = saturate(_S32);
    (&s_2)->metallic_0 = mr_3.x;
    (&s_2)->roughness_1 = mr_3.y;

#line 154
    float3 _S33 = GlowMaterial_evalEmission_0(c_13);
    (&s_2)->emission_0 = _S33;
    return s_2;
}


#line 57 "engine/render/shaders/veach/bindless.slang"
metal::raytracing::acceleration_structure<metal::raytracing::instancing> getDescriptorFromHandle_0(metal::raytracing::acceleration_structure<metal::raytracing::instancing> handle_0)
{

#line 68
    return handle_0;
}


#line 18 "engine/render/shaders/veach/atmosphere.slang"
struct AtmosphereParams_natural_0
{
    packed_float4 rayleigh_0;
    packed_float4 mie_0;
    packed_float4 ozone_0;
    packed_float4 planet_0;
};


struct _MatrixStorage_float4x4_ColMajornatural_0
{
    array<packed_float4, int(4)> data_0;
};


#line 37
struct SkyFrame_natural_0
{
    AtmosphereParams_natural_0 atmosphere_0;
    packed_float4 sun_0;
    packed_float4 camera_0;
    packed_float4 constant_0;
    _MatrixStorage_float4x4_ColMajornatural_0 inverse_view_projection_0;
    texture2d<float, access::sample> transmittance_0;
    texture2d<float, access::sample> multiscattering_0;
    texture2d<float, access::sample> sky_view_0;
    texture2d<float, access::sample> aerial_0;
    texture2d<float, access::sample> irradiance_0;
    sampler linear_clamp_0;
    uint mode_0;
    uint aerial_on_0;
    uint pad0_0;
    uint pad1_0;
};


#line 33 "engine/render/shaders/veach/realtime.slang"
struct PackedGiReservoir_natural_0
{
    packed_uint4 point_normal_0;
    packed_uint4 radiance_mw_0;
};


#line 5180 "hlsl.meta.slang"
struct MaterialRecord_natural_0
{
    uint kind_0;
    uint flags_0;
    float alpha_cutoff_0;
    float normal_scale_0;
    packed_float4 base_color_0;
    packed_float4 emission_metallic_0;
    float roughness_0;
    float emissive_weight_0;
    sampler sampler_0;
    texture2d<float, access::sample> base_color_texture_0;
    texture2d<float, access::sample> metallic_roughness_texture_0;
    texture2d<float, access::sample> normal_texture_0;
    texture2d<float, access::sample> emissive_texture_0;
    packed_float4 custom0_0;
    packed_float4 custom1_0;
};


#line 18 "engine/render/shaders/veach/scene.slang"
struct _Array_natural_vectorx3Cfloatx2C4x3E3_0
{
    array<packed_float4, int(3)> data_1;
};


#line 27
struct InstanceRecord_natural_0
{
    float device* vertices_0;
    uint device* indices_0;
    uint device* slots_0;
    uint material_base_0;
    uint flags_1;
    _Array_natural_vectorx3Cfloatx2C4x3E3_0 object_to_world_0;
    _Array_natural_vectorx3Cfloatx2C4x3E3_0 normal_to_world_0;
    uint device* meshlets_0;
    uint device* meshlet_vertices_0;
    uint device* meshlet_triangles_0;
    uint meshlet_count_0;
    uint pad_1;
    _Array_natural_vectorx3Cfloatx2C4x3E3_0 previous_object_to_world_0;
};


#line 13 "engine/render/shaders/veach/realtime.slang"
struct AliasEntry_0
{
    float threshold_0;
    uint alias_0;
    float pdf_0;
    uint pad_2;
};


#line 54
struct PackedReservoir_natural_0
{
    packed_float4 position_light_0;
    packed_float4 normal_w_0;
    packed_float4 radiance_m_0;
};


#line 54
struct RealtimeFrame_natural_0
{
    _MatrixStorage_float4x4_ColMajornatural_0 view_projection_0;
    _MatrixStorage_float4x4_ColMajornatural_0 inverse_view_projection_1;
    _MatrixStorage_float4x4_ColMajornatural_0 unjittered_view_projection_0;
    _MatrixStorage_float4x4_ColMajornatural_0 previous_view_projection_0;
    packed_float4 camera_position_0;
    packed_float4 previous_camera_position_0;
    packed_float4 reserved_0;
    packed_float4 jitter_0;
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> tlas_0;
    InstanceRecord_natural_0 device* instances_0;
    MaterialRecord_natural_0 device* materials_0;
    LightRecord_natural_0 device* lights_0;
    EmissiveTriangle_natural_0 device* emissive_0;
    texture2d<uint, access::sample> visibility_0;
    texture2d<float, access::sample> position_2;
    texture2d<float, access::sample> normal_2;
    texture2d<float, access::sample> geometric_0;
    texture2d<float, access::sample> diffuse_0;
    texture2d<float, access::sample> specular_0;
    texture2d<float, access::sample> emission_1;
    texture2d<float, access::sample> motion_0;
    texture2d<float, access::read_write> position_out_0;
    texture2d<float, access::read_write> normal_out_0;
    texture2d<float, access::read_write> geometric_out_0;
    texture2d<float, access::read_write> diffuse_out_0;
    texture2d<float, access::read_write> specular_out_0;
    texture2d<float, access::read_write> emission_out_0;
    texture2d<float, access::read_write> motion_out_0;
    texture2d<float, access::sample> previous_position_0;
    texture2d<float, access::sample> previous_normal_0;
    texture2d<float, access::sample> previous_geometric_0;
    texture2d<float, access::sample> previous_diffuse_0;
    texture2d<float, access::sample> previous_specular_0;
    texture2d<float, access::sample> radiance_1;
    texture2d<float, access::read_write> radiance_out_0;
    AliasEntry_0 device* alias_1;
    PackedReservoir_natural_0 device* temporal_out_0;
    PackedReservoir_natural_0 device* temporal_reservoirs_0;
    PackedReservoir_natural_0 device* final_out_0;
    PackedReservoir_natural_0 device* final_reservoirs_0;
    PackedReservoir_natural_0 device* previous_reservoirs_0;
    texture2d<float, access::sample> accumulation_0;
    texture2d<float, access::read_write> accumulation_out_0;
    texture2d<float, access::sample> noisy_diffuse_0;
    texture2d<float, access::read_write> noisy_diffuse_out_0;
    texture2d<float, access::sample> noisy_specular_0;
    texture2d<float, access::read_write> noisy_specular_out_0;
    texture2d<float, access::sample> previous_diffuse_history_0;
    texture2d<float, access::sample> previous_specular_history_0;
    texture2d<float, access::read_write> diffuse_history_out_0;
    texture2d<float, access::read_write> specular_history_out_0;
    texture2d<float, access::sample> diffuse_history_0;
    texture2d<float, access::sample> specular_history_0;
    texture2d<float, access::sample> previous_moments_0;
    texture2d<float, access::read_write> moments_out_0;
    texture2d<float, access::sample> moments_0;
    texture2d<float, access::sample> atrous_diffuse0_0;
    texture2d<float, access::read_write> atrous_diffuse0_out_0;
    texture2d<float, access::sample> atrous_diffuse1_0;
    texture2d<float, access::read_write> atrous_diffuse1_out_0;
    texture2d<float, access::sample> atrous_specular0_0;
    texture2d<float, access::read_write> atrous_specular0_out_0;
    texture2d<float, access::sample> atrous_specular1_0;
    texture2d<float, access::read_write> atrous_specular1_out_0;
    texture2d<float, access::sample> denoised_0;
    texture2d<float, access::read_write> denoised_out_0;
    uint device* counters_0;
    SkyFrame_natural_0 device* sky_0;
    texture2d<float, access::sample> indirect_0;
    texture2d<float, access::read_write> indirect_out_0;
    uint device* cache_keys_0;
    uint device* cache_accum_0;
    packed_uint2 device* cache_radiance_0;
    PackedGiReservoir_natural_0 device* gi_temporal_out_0;
    PackedGiReservoir_natural_0 device* gi_temporal_0;
    PackedGiReservoir_natural_0 device* gi_final_out_0;
    PackedGiReservoir_natural_0 device* gi_previous_0;
    uint device* unused1_0;
    packed_uint2 render_size_0;
    uint frame_0;
    uint flags_2;
    uint light_count_0;
    uint emissive_count_0;
    float emissive_total_weight_0;
    uint scene_flags_0;
    uint gi_0;
    uint ao_rays_0;
    float ao_radius_0;
    uint debug_skip_0;
    uint view_1;
    uint light_candidates_0;
    uint spatial_samples_0;
    uint temporal_reuse_0;
    uint accumulated_0;
    uint denoise_0;
    uint cache_capacity_0;
    float cache_scale_0;
    float cache_cell_size_0;
    float cache_cell_scale_0;
    uint cache_max_frames_0;
    uint gi_rate_0;
    uint gi_spatial_samples_0;
    uint gi_cache_bounces_0;
    uint gi_reservoirs_valid_0;
    uint gi_gather_0;
};


#line 13 "engine/render/shaders/veach/brdf.slang"
struct Frame_0
{
    float3 t_2;
    float3 b_1;
    float3 n_1;
};


Frame_0 frame_of_0(float3 n_2)
{

#line 22
    float _S34 = n_2.z;

#line 22
    float s_3;

#line 22
    if(_S34 >= 0.0f)
    {

#line 22
        s_3 = 1.0f;

#line 22
    }
    else
    {

#line 22
        s_3 = -1.0f;

#line 22
    }
    float a_0 = -1.0f / (s_3 + _S34);
    float _S35 = n_2.x;

#line 24
    float _S36 = n_2.y;

#line 24
    float b_2 = _S35 * _S36 * a_0;
    thread Frame_0 f_0;
    (&f_0)->t_2 = float3(1.0f + s_3 * _S35 * _S35 * a_0, s_3 * b_2, - s_3 * _S35);
    (&f_0)->b_1 = float3(b_2, s_3 + _S36 * _S36 * a_0, - _S36);
    (&f_0)->n_1 = n_2;
    return f_0;
}


#line 17
float3 Frame_to_local_0(const Frame_0 thread* this_0, float3 v_0)
{

#line 17
    return float3(dot(v_0, this_0->t_2), dot(v_0, this_0->b_1), dot(v_0, this_0->n_1));
}


#line 9
float luminance_0(float3 c_14)
{

#line 10
    return dot(c_14, float3(0.2125999927520752f, 0.71520000696182251f, 0.072200000286102295f));
}


#line 32
struct Brdf_0
{
    float3 diffuse_1;
    float3 f0_0;
    float alpha_1;
    float specular_probability_0;
};


#line 193 "engine/render/shaders/veach/realtime.slang"
struct GSurface_0
{
    bool valid_0;
    float3 position_3;
    float depth_0;
    float3 normal_3;
    float roughness_2;
    float3 geometric_1;
    float metallic_1;
    float3 diffuse_2;
    float3 f0_1;
    float3 emission_2;
};


#line 278
Brdf_0 brdf_of_surface_0(const GSurface_0 thread* s_4, float n_dot_v_0)
{

#line 279
    thread Brdf_0 b_3;
    (&b_3)->diffuse_1 = s_4->diffuse_2;
    (&b_3)->f0_0 = s_4->f0_1;
    float _S37 = max(s_4->roughness_2, 0.019999999552965164f);
    (&b_3)->alpha_1 = _S37 * _S37;
    float spec_0 = luminance_0(s_4->f0_1 + (float3(1.0f)  - s_4->f0_1) * float3(pow(1.0f - n_dot_v_0, 5.0f)) );

    float _S38 = spec_0 + luminance_0(s_4->diffuse_2);

#line 286
    float _S39;

#line 286
    if(_S38 > 0.0f)
    {

#line 286
        _S39 = clamp(spec_0 / _S38, 0.10000000149011612f, 0.89999997615814209f);

#line 286
    }
    else
    {

#line 286
        _S39 = 0.5f;

#line 286
    }

#line 286
    (&b_3)->specular_probability_0 = _S39;
    return b_3;
}


#line 62 "engine/render/shaders/veach/restir.slang"
struct Domain_0
{
    bool valid_1;
    float3 p_0;
    float3 g_0;
    Frame_0 frame_1;
    Brdf_0 b_4;
    float3 v_1;
};


#line 71
Domain_0 domain_of_0(const GSurface_0 thread* s_5, float3 camera_1)
{

#line 72
    thread Domain_0 d_0;
    (&d_0)->p_0 = s_5->position_3;
    (&d_0)->g_0 = s_5->geometric_1;
    Frame_0 _S40 = frame_of_0(s_5->normal_3);

#line 75
    (&d_0)->frame_1 = _S40;
    float3 _S41 = normalize(camera_1 - s_5->position_3);

#line 76
    thread Frame_0 _S42 = _S40;

#line 76
    float3 _S43 = Frame_to_local_0(&_S42, _S41);

#line 76
    (&d_0)->v_1 = _S43;

#line 76
    bool _S44;
    if(s_5->valid_0)
    {

#line 77
        _S44 = ((&d_0)->v_1.z) > 0.0f;

#line 77
    }
    else
    {

#line 77
        _S44 = false;

#line 77
    }

#line 77
    (&d_0)->valid_1 = _S44;

#line 77
    Brdf_0 _S45 = brdf_of_surface_0(s_5, max((&d_0)->v_1.z, 9.9999997473787516e-05f));
    (&d_0)->b_4 = _S45;
    return d_0;
}


#line 171 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
struct GiReservoir_0
{
    float3 point_0;
    float3 normal_4;
    uint normal_code_0;
    bool sky_1;
    float3 radiance_2;
    float w_sum_0;
    float M_0;
};


#line 181
GiReservoir_0 empty_gi_0()
{

#line 182
    thread GiReservoir_0 r_0;
    float3 _S46 = float3(0.0f, 0.0f, 0.0f);

#line 183
    (&r_0)->point_0 = _S46;
    (&r_0)->normal_4 = float3(0.0f, 0.0f, 1.0f);
    (&r_0)->normal_code_0 = 0U;
    (&r_0)->sky_1 = false;
    (&r_0)->radiance_2 = _S46;
    (&r_0)->w_sum_0 = 0.0f;
    (&r_0)->M_0 = 0.0f;
    return r_0;
}


#line 30 "engine/render/shaders/veach/realtime.slang"
struct PackedGiReservoir_0
{
    uint4 point_normal_0;
    uint4 radiance_mw_0;
};


#line 222 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
PackedGiReservoir_0 pack_gi_0(const GiReservoir_0 thread* r_1)
{

#line 223
    thread PackedGiReservoir_0 p_1;
    uint3 _S47 = (as_type<uint3>((r_1->point_0)));

#line 224
    uint _S48;

#line 224
    if(r_1->sky_1)
    {

#line 224
        _S48 = 4294967295U;

#line 224
    }
    else
    {

#line 224
        _S48 = r_1->normal_code_0;

#line 224
    }

#line 224
    (&p_1)->point_normal_0 = uint4(_S47, _S48);
    float3 _S49 = min(r_1->radiance_2, float3(60000.0f) );
    (&p_1)->radiance_mw_0 = uint4((uint(as_type<ushort>((half)((_S49.x))))) | ((uint(as_type<ushort>((half)((_S49.y))))) << 16U), (uint(as_type<ushort>((half)((_S49.z))))) | ((uint(as_type<ushort>((half)((r_1->M_0))))) << 16U), (as_type<uint>((r_1->w_sum_0))), 0U);

    return p_1;
}


#line 4 "engine/render/shaders/veach/random.slang"
uint pcg_0(uint v_2)
{

#line 5
    uint state_0 = v_2 * 747796405U + 2891336453U;
    uint word_0 = ((state_0 >> ((state_0 >> 28U) + 4U)) ^ state_0) * 277803737U;
    return (word_0 >> 22U) ^ word_0;
}

struct Rng_0
{
    uint state_1;
};


#line 10
Rng_0 Rng_x24init_0(uint state_2)
{

#line 10
    thread Rng_0 _S50;
    (&_S50)->state_1 = state_2;

#line 10
    return _S50;
}


#line 335 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
bool gathers_this_frame_0(uint2 pixel_0, uint frame_2, uint rate_0)
{

#line 336
    if(rate_0 <= 1U)
    {

#line 337
        return true;
    }
    if(rate_0 == 2U)
    {

#line 340
        return ((pixel_0.x + pixel_0.y + frame_2) & 1U) == 0U;
    }
    array<uint, int(4)> order_0 = { { 0U, 3U, 1U, 2U } };
    return (((pixel_0.x) & 1U) | (((pixel_0.y) & 1U) << 1U)) == (order_0[frame_2 & 3U]);
}


#line 12 "engine/render/shaders/veach/random.slang"
float Rng_next_0(Rng_0 thread* this_1)
{

#line 13
    uint _S51 = pcg_0(this_1->state_1);

#line 13
    this_1->state_1 = _S51;
    return float(_S51 >> 8U) * 5.9604644775390625e-08f;
}


#line 18 "engine/render/shaders/veach/brdf.slang"
float3 Frame_to_world_0(const Frame_0 thread* this_2, float3 v_3)
{

#line 18
    return float3(v_3.x)  * this_2->t_2 + float3(v_3.y)  * this_2->b_1 + float3(v_3.z)  * this_2->n_1;
}


#line 61 "engine/render/shaders/veach/diffuse_path.slang"
float3 cosine_direction_0(const Frame_0 thread* frame_3, Rng_0 thread* rng_0, float thread* pdf_1)
{

#line 62
    float _S52 = Rng_next_0(rng_0);

#line 62
    float _S53 = Rng_next_0(rng_0);
    float r_2 = sqrt(_S52);
    float _S54 = 6.2831854820251465f * _S53;

#line 64
    float _S55 = sqrt(1.0f - _S52);

#line 64
    float3 l_0 = float3(r_2 * cos(_S54), r_2 * sin(_S54), _S55);
    *pdf_1 = _S55 / 3.1415927410125732f;

#line 65
    float3 _S56 = Frame_to_world_0(frame_3, l_0);
    return _S56;
}


#line 320 "engine/render/shaders/veach/scene.slang"
float3 offset_origin_0(float3 p_2, float3 geometric_2, float3 direction_1)
{

#line 320
    float _S57;
    if((dot(geometric_2, direction_1)) >= 0.0f)
    {

#line 321
        _S57 = 0.0010000000474974513f;

#line 321
    }
    else
    {

#line 321
        _S57 = -0.0010000000474974513f;

#line 321
    }

#line 321
    return p_2 + geometric_2 * float3(_S57) ;
}


#line 17 "engine/render/shaders/veach/material.slang"
SurfaceInput_0 SurfaceInput_x24init_0(float3 position_4, float3 geometric_normal_1, float3 normal_5, float4 tangent_1, float2 uv_2, float2 uv_dx_1, float2 uv_dy_1, float3 view_2)
{

#line 17
    thread SurfaceInput_0 _S58;
    (&_S58)->position_0 = position_4;
    (&_S58)->geometric_normal_0 = geometric_normal_1;
    (&_S58)->normal_0 = normal_5;
    (&_S58)->tangent_0 = tangent_1;
    (&_S58)->uv_0 = uv_2;
    (&_S58)->uv_dx_0 = uv_dx_1;
    (&_S58)->uv_dy_0 = uv_dy_1;
    (&_S58)->view_0 = view_2;

#line 17
    return _S58;
}


#line 57
MaterialContext_0 MaterialContext_x24init_0(const SurfaceInput_0 thread* surface_1, const MaterialRecord_natural_0 thread* record_1)
{

#line 57
    float4 _S59 = float4(record_1->base_color_0) ;

#line 57
    float4 _S60 = float4(record_1->emission_metallic_0) ;

#line 57
    float4 _S61 = float4(record_1->custom0_0) ;

#line 57
    float4 _S62 = float4(record_1->custom1_0) ;

#line 57
    thread MaterialContext_0 _S63;
    (&_S63)->surface_0 = *surface_1;

#line 58
    (&(&_S63)->record_0)->kind_0 = record_1->kind_0;

#line 58
    (&(&_S63)->record_0)->flags_0 = record_1->flags_0;

#line 58
    (&(&_S63)->record_0)->alpha_cutoff_0 = record_1->alpha_cutoff_0;

#line 58
    (&(&_S63)->record_0)->normal_scale_0 = record_1->normal_scale_0;

#line 58
    (&(&_S63)->record_0)->base_color_0 = _S59;

#line 58
    (&(&_S63)->record_0)->emission_metallic_0 = _S60;

#line 58
    (&(&_S63)->record_0)->roughness_0 = record_1->roughness_0;

#line 58
    (&(&_S63)->record_0)->emissive_weight_0 = record_1->emissive_weight_0;

#line 58
    (&(&_S63)->record_0)->sampler_0 = record_1->sampler_0;

#line 58
    (&(&_S63)->record_0)->base_color_texture_0 = record_1->base_color_texture_0;

#line 58
    (&(&_S63)->record_0)->metallic_roughness_texture_0 = record_1->metallic_roughness_texture_0;

#line 58
    (&(&_S63)->record_0)->normal_texture_0 = record_1->normal_texture_0;

#line 58
    (&(&_S63)->record_0)->emissive_texture_0 = record_1->emissive_texture_0;

#line 58
    (&(&_S63)->record_0)->custom0_0 = _S61;

#line 58
    (&(&_S63)->record_0)->custom1_0 = _S62;

#line 57
    return _S63;
}


#line 57
MaterialContext_0 MaterialContext_x24init_1(const SurfaceInput_0 thread* surface_2, const MaterialRecord_0 thread* record_2)
{

#line 57
    thread MaterialContext_0 _S64;
    (&_S64)->surface_0 = *surface_2;
    (&_S64)->record_0 = *record_2;

#line 57
    return _S64;
}


#line 13 "build/release/apps/lab/generated/veach/materials_generated.slang"
float evaluateAlpha_0(uint kind_2, const MaterialContext_0 thread* c_15)
{

#line 14
    switch(kind_2)
    {
    case 1U:
        {

#line 14
            float4 _S65 = IMaterial_evalBaseColorx24defaultImpl_1(c_15);
            return _S65.w;
        }
    default:
        {

#line 15
            float4 _S66 = IMaterial_evalBaseColorx24defaultImpl_0(c_15);
            return _S66.w;
        }
    }

#line 16
}


#line 28 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
struct RealtimeParams_0
{
    RealtimeFrame_natural_0 device* frame_4;
    uint argument_0;
    uint pad_3;
};


#line 61 "engine/render/shaders/veach/scene.slang"
struct SceneData_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> tlas_1;
    InstanceRecord_natural_0 device* instances_1;
    MaterialRecord_natural_0 device* materials_1;
    LightRecord_natural_0 device* lights_1;
    EmissiveTriangle_natural_0 device* emissive_1;
    uint light_count_1;
    uint emissive_count_1;
    float emissive_total_weight_1;
    uint debug_skip_1;
    uint flags_3;
};


#line 5653 "core.meta.slang"
struct KernelContext_0
{
    RealtimeParams_0 constant* realtime_params_0;
    SceneData_0 veach_scene_0;
    uint _S67;
};


#line 5653
uint slot_of_0(InstanceRecord_natural_0 device* _S68, uint _S69, uint _S70)
{

#line 106 "engine/render/shaders/veach/scene.slang"
    return (_S68+_S69)->slots_0[_S70];
}


#line 106
uint vertex_index_0(InstanceRecord_natural_0 device* _S71, uint _S72, uint _S73)
{

#line 103
    return (_S71+_S72)->indices_0[_S73];
}


#line 103
float vertex_float_0(InstanceRecord_natural_0 device* _S74, uint _S75, uint _S76)
{

#line 100
    return (_S74+_S75)->vertices_0[_S76];
}


#line 200
bool covered_0(uint instance_index_0, uint primitive_1, float2 bary_0, Rng_0 thread* rng_1, KernelContext_0 thread* kernelContext_0)
{

#line 201
    InstanceRecord_natural_0 device* _S77 = (&kernelContext_0->veach_scene_0)->instances_1;
    MaterialRecord_natural_0 device* _S78 = (&kernelContext_0->veach_scene_0)->materials_1;

#line 202
    uint _S79 = ((&kernelContext_0->veach_scene_0)->instances_1+instance_index_0)->material_base_0;

#line 202
    uint _S80 = slot_of_0((&kernelContext_0->veach_scene_0)->instances_1, instance_index_0, primitive_1);

#line 202
    thread MaterialRecord_natural_0 _S81 = _S78[_S79 + _S80];

#line 202
    uint _S82 = (&_S81)->flags_0;
    if((((&_S81)->flags_0) & 48U) == 0U)
    {

#line 204
        return true;
    }
    float _S83 = bary_0.x;

#line 206
    float _S84 = bary_0.y;

#line 206
    float3 _S85 = float3(1.0f - _S83 - _S84, _S83, _S84);
    float2 _S86 = float2(0.0f, 0.0f);

#line 207
    uint k_0 = 0U;

#line 207
    float2 uv_3 = _S86;
    for(;;)
    {

#line 208
        if(k_0 < 3U)
        {
        }
        else
        {

#line 208
            break;
        }

#line 208
        uint _S87 = vertex_index_0(_S77, instance_index_0, primitive_1 * 3U + k_0);
        uint b_5 = _S87 * 12U + 10U;

#line 209
        float _S88 = vertex_float_0(_S77, instance_index_0, b_5);

#line 209
        float _S89 = vertex_float_0(_S77, instance_index_0, b_5 + 1U);
        float2 uv_4 = uv_3 + float2(_S88, _S89) * float2(_S85[k_0]) ;

#line 208
        k_0 = k_0 + 1U;

#line 208
        uv_3 = uv_4;

#line 208
    }

#line 18 "engine/render/shaders/veach/material.slang"
    float3 _S90 = float3(0.0f) ;



    float2 _S91 = float2(0.0f) ;

#line 212 "engine/render/shaders/veach/scene.slang"
    thread SurfaceInput_0 surface_3 = SurfaceInput_x24init_0(_S90, _S90, _S90, float4(0.0f) , _S91, _S91, _S91, _S90);
    (&surface_3)->uv_0 = uv_3;

#line 213
    thread SurfaceInput_0 _S92 = surface_3;

#line 213
    MaterialContext_0 _S93 = MaterialContext_x24init_0(&_S92, &_S81);

#line 213
    thread MaterialContext_0 _S94 = _S93;

#line 213
    float _S95 = evaluateAlpha_0((&_S81)->kind_0, &_S94);


    if((_S82 & 16U) != 0U)
    {

#line 217
        return _S95 >= ((&_S81)->alpha_cutoff_0);
    }
    float _S96 = Rng_next_0(rng_1);

#line 219
    return _S96 < _S95;
}

struct Trace_0
{
    bool hit_0;
    uint instance_1;
    uint primitive_2;
    float2 bary_1;
    float t_3;
};


#line 288
Trace_0 trace_closest_0(float3 origin_0, float3 direction_2, float tmax_0, Rng_0 thread* rng_2, KernelContext_0 thread* kernelContext_1)
{

#line 289
    thread Trace_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->instance_1 = 0U;
    (&result_0)->primitive_2 = 0U;
    (&result_0)->bary_1 = float2(0.0f, 0.0f);
    (&result_0)->t_3 = 0.0f;
    thread RayDesc ray_0;
    (&ray_0)->Origin = origin_0;
    (&ray_0)->Direction = direction_2;
    (&ray_0)->TMin = 0.0f;
    (&ray_0)->TMax = tmax_0;

    metal::raytracing::acceleration_structure<metal::raytracing::instancing> _S97 = getDescriptorFromHandle_0((&kernelContext_1->veach_scene_0)->tlas_1);

#line 300
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;
    (&query_0)->reset({ (ray_0.Origin), (ray_0.Direction), (ray_0.TMin), (ray_0.TMax) }, (_S97), (255U), _slang_ray_flags_to_intersection_params((0U)));
    for(;;)
    {

#line 302
        bool _S98 = ((&query_0)->next());

#line 302
        if(_S98)
        {
        }
        else
        {

#line 302
            break;
        }

#line 303
        uint _S99 = (
                        _slang_candidate_intersection_type_to_uint((&query_0)->get_candidate_intersection_type())
                    );

#line 303
        bool _S100;

#line 303
        if(_S99 == 0U)
        {

#line 304
            uint _S101 = ((&query_0)->get_candidate_instance_id());

#line 304
            uint _S102 = ((&query_0)->get_candidate_primitive_id());
            float2 _S103 = ((&query_0)->get_candidate_triangle_barycentric_coord());

#line 305
            bool _S104 = covered_0(_S101, _S102, _S103, rng_2, kernelContext_1);

#line 305
            _S100 = _S104;

#line 303
        }
        else
        {

#line 303
            _S100 = false;

#line 303
        }

#line 303
        if(_S100)
        {

            (&query_0)->commit_triangle_intersection();

#line 303
        }

#line 302
    }

#line 309
    uint _S105 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 309
    if(_S105 == 1U)
    {

#line 310
        (&result_0)->hit_0 = true;
        uint _S106 = ((&query_0)->get_committed_instance_id());

#line 311
        (&result_0)->instance_1 = _S106;
        uint _S107 = ((&query_0)->get_committed_primitive_id());

#line 312
        (&result_0)->primitive_2 = _S107;
        float2 _S108 = ((&query_0)->get_committed_triangle_barycentric_coord());

#line 313
        (&result_0)->bary_1 = _S108;
        float _S109 = ((&query_0)->get_committed_distance());

#line 314
        (&result_0)->t_3 = _S109;

#line 309
    }

#line 316
    return result_0;
}


#line 73 "engine/render/shaders/veach/atmosphere.slang"
float ray_sphere_0(float3 origin_1, float3 direction_3, float radius_0)
{

#line 74
    float b_6 = dot(origin_1, direction_3);

    float discriminant_0 = b_6 * b_6 - (dot(origin_1, origin_1) - radius_0 * radius_0);
    if(discriminant_0 < 0.0f)
    {

#line 78
        return -1.0f;
    }
    float s_6 = sqrt(discriminant_0);
    float _S110 = - b_6;

#line 81
    float t0_0 = _S110 - s_6;
    float t1_0 = _S110 + s_6;

#line 82
    float _S111;
    if(t0_0 >= 0.0f)
    {

#line 83
        _S111 = t0_0;

#line 83
    }
    else
    {

#line 83
        if(t1_0 >= 0.0f)
        {

#line 83
            _S111 = t1_0;

#line 83
        }
        else
        {

#line 83
            _S111 = -1.0f;

#line 83
        }

#line 83
    }

#line 83
    return _S111;
}



float2 transmittance_uv_0(const AtmosphereParams_natural_0 thread* a_1, float r_3, float mu_0)
{

#line 88
    float4 _S112 = float4(a_1->planet_0) ;
    float bottom_0 = _S112.x;
    float top_0 = _S112.y;
    float _S113 = top_0 * top_0;

#line 91
    float _S114 = bottom_0 * bottom_0;

#line 91
    float h_0 = sqrt(max(_S113 - _S114, 0.0f));
    float _S115 = r_3 * r_3;

#line 92
    float rho_0 = sqrt(max(_S115 - _S114, 0.0f));


    float d_min_0 = top_0 - r_3;

    return float2((max(0.0f, - r_3 * mu_0 + sqrt(max(_S115 * (mu_0 * mu_0 - 1.0f) + _S113, 0.0f))) - d_min_0) / max(rho_0 + h_0 - d_min_0, 9.99999997475242708e-07f), rho_0 / h_0);
}


#line 119 "engine/render/shaders/veach/scene.slang"
struct TriangleHit_0
{
    SurfaceInput_0 surface_4;
    MaterialRecord_0 material_0;
    float area_0;
    bool front_0;
};


struct TriangleRef_0
{
    uint3 vertices_1;
    uint slot_0;
};


#line 128
TriangleRef_0 triangle_of_0(InstanceRecord_natural_0 device* _S116, uint _S117, uint _S118)
{

#line 134
    thread TriangleRef_0 t_4;
    uint _S119 = _S118 * 3U;

#line 135
    uint _S120 = vertex_index_0(_S116, _S117, _S119);

#line 135
    uint _S121 = vertex_index_0(_S116, _S117, _S119 + 1U);

#line 135
    uint _S122 = vertex_index_0(_S116, _S117, _S119 + 2U);

#line 135
    (&t_4)->vertices_1 = uint3(_S120, _S121, _S122);

#line 135
    uint _S123 = slot_of_0(_S116, _S117, _S118);

    (&t_4)->slot_0 = _S123;
    return t_4;
}


#line 91
struct Corner_0
{
    float3 position_5;
    float3 normal_6;
    float4 tangent_2;
    float2 uv_5;
};


#line 91
Corner_0 fetch_0(InstanceRecord_natural_0 device* _S124, uint _S125, uint _S126)
{

#line 110
    uint b_7 = _S126 * 12U;
    thread Corner_0 c_16;

#line 111
    float _S127 = vertex_float_0(_S124, _S125, b_7);

#line 111
    float _S128 = vertex_float_0(_S124, _S125, b_7 + 1U);

#line 111
    float _S129 = vertex_float_0(_S124, _S125, b_7 + 2U);
    (&c_16)->position_5 = float3(_S127, _S128, _S129);

#line 112
    float _S130 = vertex_float_0(_S124, _S125, b_7 + 3U);

#line 112
    float _S131 = vertex_float_0(_S124, _S125, b_7 + 4U);

#line 112
    float _S132 = vertex_float_0(_S124, _S125, b_7 + 5U);
    (&c_16)->normal_6 = float3(_S130, _S131, _S132);

#line 113
    float _S133 = vertex_float_0(_S124, _S125, b_7 + 6U);

#line 113
    float _S134 = vertex_float_0(_S124, _S125, b_7 + 7U);

#line 113
    float _S135 = vertex_float_0(_S124, _S125, b_7 + 8U);

#line 113
    float _S136 = vertex_float_0(_S124, _S125, b_7 + 9U);
    (&c_16)->tangent_2 = float4(_S133, _S134, _S135, _S136);

#line 114
    float _S137 = vertex_float_0(_S124, _S125, b_7 + 10U);

#line 114
    float _S138 = vertex_float_0(_S124, _S125, b_7 + 11U);
    (&c_16)->uv_5 = float2(_S137, _S138);
    return c_16;
}


#line 116
float3 to_world_0(InstanceRecord_natural_0 device* _S139, uint _S140, float3 _S141, float _S142)
{

#line 87
    float4 v_4 = float4(_S141, _S142);
    return float3(dot(float4((&(_S139+_S140)->object_to_world_0)->data_1[int(0)]) , v_4), dot(float4((&(_S139+_S140)->object_to_world_0)->data_1[int(1)]) , v_4), dot(float4((&(_S139+_S140)->object_to_world_0)->data_1[int(2)]) , v_4));
}


#line 88
float3 to_world_1(InstanceRecord_natural_0 device* _S143, uint _S144, float3 _S145, float _S146)
{

#line 87
    float4 v_5 = float4(_S145, _S146);
    return float3(dot(float4((&(_S143+_S144)->normal_to_world_0)->data_1[int(0)]) , v_5), dot(float4((&(_S143+_S144)->normal_to_world_0)->data_1[int(1)]) , v_5), dot(float4((&(_S143+_S144)->normal_to_world_0)->data_1[int(2)]) , v_5));
}


#line 88
TriangleHit_0 surface_of_0(InstanceRecord_natural_0 device* _S147, uint _S148, const TriangleRef_0 thread* _S149, float2 _S150, float3 _S151, KernelContext_0 thread* kernelContext_2)
{

#line 88
    InstanceRecord_natural_0 device* _S152 = _S147+_S148;

#line 88
    uint3 _S153 = _S149->vertices_1;

#line 88
    Corner_0 _S154 = fetch_0(_S147, _S148, _S149->vertices_1.x);

#line 88
    Corner_0 _S155 = fetch_0(_S147, _S148, _S153.y);

#line 88
    Corner_0 _S156 = fetch_0(_S147, _S148, _S153.z);

#line 170
    float _S157 = _S150.x;

#line 170
    float _S158 = _S150.y;

#line 170
    float _S159 = 1.0f - _S157 - _S158;

#line 170
    float3 _S160 = to_world_0(_S147, _S148, _S154.position_5, 1.0f);

#line 170
    float3 _S161 = to_world_0(_S147, _S148, _S155.position_5, 1.0f);

#line 170
    float3 _S162 = to_world_0(_S147, _S148, _S156.position_5, 1.0f);

#line 175
    float3 cross_edges_0 = cross(_S161 - _S160, _S162 - _S160);

#line 174
    thread TriangleHit_0 hit_1;

    (&hit_1)->area_0 = 0.5f * length(cross_edges_0);
    float3 geometric_3 = normalize(cross_edges_0);

#line 177
    float3 _S163 = float3(_S159) ;

#line 177
    float3 _S164 = float3(_S157) ;

#line 177
    float3 _S165 = float3(_S158) ;
    float3 _S166 = _S160 * _S163 + _S161 * _S164 + _S162 * _S165;

#line 178
    (&(&hit_1)->surface_4)->position_0 = _S166;
    float3 _S167 = normalize(_S151 - _S166);

#line 179
    (&(&hit_1)->surface_4)->view_0 = _S167;

#line 179
    float3 _S168 = to_world_1(_S147, _S148, _S154.normal_6 * _S163 + _S155.normal_6 * _S164 + _S156.normal_6 * _S165, 0.0f);
    float3 normal_7 = normalize(_S168);
    float4 t_5 = _S154.tangent_2 * float4(_S159)  + _S155.tangent_2 * float4(_S157)  + _S156.tangent_2 * float4(_S158) ;

#line 181
    float3 _S169 = to_world_0(_S147, _S148, t_5.xyz, 0.0f);
    (&(&hit_1)->surface_4)->tangent_0 = float4(_S169, t_5.w);
    (&(&hit_1)->surface_4)->uv_0 = _S154.uv_5 * float2(_S159)  + _S155.uv_5 * float2(_S157)  + _S156.uv_5 * float2(_S158) ;
    float2 _S170 = float2(0.0f, 0.0f);

#line 184
    (&(&hit_1)->surface_4)->uv_dx_0 = _S170;
    (&(&hit_1)->surface_4)->uv_dy_0 = _S170;

    bool _S171 = (dot(geometric_3, _S167)) >= 0.0f;

#line 187
    (&hit_1)->front_0 = _S171;

#line 187
    float3 geometric_4;

#line 187
    float3 normal_8;
    if(!_S171)
    {
        float3 _S172 = - normal_7;

#line 190
        geometric_4 = - geometric_3;

#line 190
        normal_8 = _S172;

#line 188
    }
    else
    {

#line 188
        geometric_4 = geometric_3;

#line 188
        normal_8 = normal_7;

#line 188
    }



    (&(&hit_1)->surface_4)->geometric_normal_0 = geometric_4;
    (&(&hit_1)->surface_4)->normal_0 = normal_8;
    MaterialRecord_natural_0 _S173 = (&kernelContext_2->veach_scene_0)->materials_1[_S152->material_base_0 + _S149->slot_0];

#line 194
    float4 _S174 = float4(_S173.base_color_0) ;

#line 194
    float4 _S175 = float4(_S173.emission_metallic_0) ;

#line 194
    float4 _S176 = float4(_S173.custom0_0) ;

#line 194
    float4 _S177 = float4(_S173.custom1_0) ;

#line 194
    (&(&hit_1)->material_0)->kind_0 = _S173.kind_0;

#line 194
    (&(&hit_1)->material_0)->flags_0 = _S173.flags_0;

#line 194
    (&(&hit_1)->material_0)->alpha_cutoff_0 = _S173.alpha_cutoff_0;

#line 194
    (&(&hit_1)->material_0)->normal_scale_0 = _S173.normal_scale_0;

#line 194
    (&(&hit_1)->material_0)->base_color_0 = _S174;

#line 194
    (&(&hit_1)->material_0)->emission_metallic_0 = _S175;

#line 194
    (&(&hit_1)->material_0)->roughness_0 = _S173.roughness_0;

#line 194
    (&(&hit_1)->material_0)->emissive_weight_0 = _S173.emissive_weight_0;

#line 194
    (&(&hit_1)->material_0)->sampler_0 = _S173.sampler_0;

#line 194
    (&(&hit_1)->material_0)->base_color_texture_0 = _S173.base_color_texture_0;

#line 194
    (&(&hit_1)->material_0)->metallic_roughness_texture_0 = _S173.metallic_roughness_texture_0;

#line 194
    (&(&hit_1)->material_0)->normal_texture_0 = _S173.normal_texture_0;

#line 194
    (&(&hit_1)->material_0)->emissive_texture_0 = _S173.emissive_texture_0;

#line 194
    (&(&hit_1)->material_0)->custom0_0 = _S176;

#line 194
    (&(&hit_1)->material_0)->custom1_0 = _S177;
    return hit_1;
}


#line 159
TriangleHit_0 surface_at_0(uint instance_index_1, uint primitive_3, float2 bary_2, float3 view_origin_0, KernelContext_0 thread* kernelContext_3)
{

#line 160
    InstanceRecord_natural_0 device* _S178 = (&kernelContext_3->veach_scene_0)->instances_1;

#line 160
    TriangleRef_0 _S179 = triangle_of_0((&kernelContext_3->veach_scene_0)->instances_1, instance_index_1, primitive_3);

#line 160
    thread TriangleRef_0 _S180 = _S179;

#line 160
    TriangleHit_0 _S181 = surface_of_0(_S178, instance_index_1, &_S180, bary_2, view_origin_0, kernelContext_3);
    return _S181;
}


#line 193 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
float2 sign_not_zero_0(float2 v_6)
{

#line 193
    float _S182;
    if((v_6.x) >= 0.0f)
    {

#line 194
        _S182 = 1.0f;

#line 194
    }
    else
    {

#line 194
        _S182 = -1.0f;

#line 194
    }

#line 194
    float _S183;

#line 194
    if((v_6.y) >= 0.0f)
    {

#line 194
        _S183 = 1.0f;

#line 194
    }
    else
    {

#line 194
        _S183 = -1.0f;

#line 194
    }

#line 194
    return float2(_S182, _S183);
}

float2 oct_encode_0(float3 n_3)
{

#line 198
    float _S184 = n_3.z;

#line 198
    float2 p_3 = n_3.xy / float2((abs(n_3.x) + abs(n_3.y) + abs(_S184))) ;

#line 198
    float2 _S185;
    if(_S184 >= 0.0f)
    {

#line 199
        _S185 = p_3;

#line 199
    }
    else
    {

#line 199
        _S185 = (float2(1.0f)  - abs(p_3.yx)) * sign_not_zero_0(p_3);

#line 199
    }

#line 199
    return _S185;
}

float3 oct_decode_0(float2 e_2)
{

#line 203
    float3 _S186 = float3(e_2, 1.0f - abs(e_2.x) - abs(e_2.y));

#line 203
    thread float3 n_4 = _S186;
    if((_S186.z) < 0.0f)
    {

#line 205
        n_4.xy = (float2(1.0f)  - abs(n_4.yx)) * sign_not_zero_0(n_4.xy);

#line 204
    }


    return normalize(n_4);
}

float3 decode_normal_0(uint code_0)
{

#line 211
    return oct_decode_0(float2(float(code_0 & 65535U), float(code_0 >> 16U)) / float2(65535.0f)  * float2(2.0f)  - float2(1.0f) );
}



void set_normal_0(GiReservoir_0 thread* r_4, float3 n_5)
{

#line 216
    float2 _S187 = float2(0.5f) ;
    uint2 _S188 = uint2(saturate(oct_encode_0(n_5) * _S187 + _S187) * float2(65535.0f)  + _S187);
    uint _S189 = (_S188.x) | ((_S188.y) << 16U);

#line 218
    r_4->normal_code_0 = _S189;
    r_4->normal_4 = decode_normal_0(_S189);
    return;
}


#line 14 "engine/render/shaders/veach/radiance_cache.slang"
struct RadianceCache_0
{
    uint device* keys_0;
    uint device* accum_0;
    packed_uint2 device* radiance_3;
    float3 camera_2;
    uint capacity_0;
    float scale_0;
    float cell_size_0;
    float cell_scale_0;
    uint max_frames_0;
    uint device* stats_0;
};


#line 65
uint cache_level_0(const RadianceCache_0 thread* c_17, float3 p_4)
{
    return min(uint(max(floor(log2(max(length(p_4 - c_17->camera_2) * c_17->cell_scale_0 / c_17->cell_size_0, 1.0f))), 0.0f)), 15U);
}


#line 54
uint normal_bucket_0(float3 n_6)
{

#line 55
    float3 a_2 = abs(n_6);
    float _S190 = a_2.x;

#line 56
    float _S191 = a_2.y;

#line 56
    bool _S192;

#line 56
    if(_S190 >= _S191)
    {

#line 56
        _S192 = _S190 >= (a_2.z);

#line 56
    }
    else
    {

#line 56
        _S192 = false;

#line 56
    }

#line 56
    uint _S193;

#line 56
    if(_S192)
    {

#line 57
        if((n_6.x) >= 0.0f)
        {

#line 57
            _S193 = 0U;

#line 57
        }
        else
        {

#line 57
            _S193 = 1U;

#line 57
        }

#line 57
        return _S193;
    }
    if(_S191 >= (a_2.z))
    {

#line 60
        if((n_6.y) >= 0.0f)
        {

#line 60
            _S193 = 2U;

#line 60
        }
        else
        {

#line 60
            _S193 = 3U;

#line 60
        }

#line 60
        return _S193;
    }
    if((n_6.z) >= 0.0f)
    {

#line 62
        _S193 = 4U;

#line 62
    }
    else
    {

#line 62
        _S193 = 5U;

#line 62
    }

#line 62
    return _S193;
}


#line 70
struct CacheKey_0
{
    uint slot_1;
    uint checksum_0;
};


#line 75
CacheKey_0 cache_key_0(const RadianceCache_0 thread* c_18, float3 p_5, float3 n_7, uint level_0, uint kind_3)
{
    int3 _S194 = int3(floor(p_5 / float3((c_18->cell_size_0 * float(1U << level_0))) ));
    uint tag_0 = (kind_3 * 16U + level_0) * 8U + normal_bucket_0(n_7);
    uint _S195 = uint(_S194.x);

#line 79
    uint h_1 = pcg_0(uint(_S194.z) ^ (pcg_0(uint(_S194.y) ^ (pcg_0(_S195 ^ (pcg_0(tag_0)))))));
    thread CacheKey_0 k_1;
    (&k_1)->slot_1 = h_1 & (c_18->capacity_0 - 1U);
    (&k_1)->checksum_0 = (pcg_0((h_1 ^ 2654435769U) ^ (pcg_0(_S195 + tag_0)))) | 1U;
    return k_1;
}

float3 unpack_cache_radiance_0(uint2 v_7)
{

#line 87
    uint _S196 = v_7.x;

#line 87
    return float3((float(as_type<half>((ushort)((_S196))))), (float(as_type<half>((ushort)((_S196 >> 16U))))), (float(as_type<half>((ushort)((v_7.y))))));
}




bool cache_lookup_0(const RadianceCache_0 thread* c_19, float3 p_6, float3 n_8, float3 thread* radiance_4, uint thread* level_offset_0, uint kind_4)
{
    *radiance_4 = float3(0.0f, 0.0f, 0.0f);
    *level_offset_0 = 0U;

#line 96
    uint _S197 = cache_level_0(c_19, p_6);

#line 96
    uint l_1 = _S197;

    for(;;)
    {

#line 98
        if(l_1 <= (min(_S197 + 2U, 15U)))
        {
        }
        else
        {

#line 98
            break;
        }

#line 99
        *level_offset_0 = l_1 - _S197;

#line 99
        CacheKey_0 _S198 = cache_key_0(c_19, p_6, n_8, l_1, kind_4);

#line 99
        uint i_0 = 0U;

        for(;;)
        {

#line 101
            if(i_0 < 8U)
            {
            }
            else
            {

#line 101
                break;
            }

#line 102
            uint slot_2 = (_S198.slot_1 + i_0) & (c_19->capacity_0 - 1U);
            if((*(c_19->keys_0+slot_2)) == (_S198.checksum_0))
            {

#line 103
                uint2 _S199 = uint2(*(c_19->radiance_3+slot_2)) ;

                if(((_S199.y) >> 24U) > 0U)
                {

#line 106
                    *radiance_4 = unpack_cache_radiance_0(_S199);
                    return true;
                }
                break;
            }

#line 101
            i_0 = i_0 + 1U;

#line 101
        }

#line 98
        l_1 = l_1 + 1U;

#line 98
    }

#line 113
    return false;
}


#line 18393 "hlsl.meta.slang"
uint4 WaveActiveBallot_0(bool condition_0)
{

#line 18402
    ulong _S200 = (uint64_t(simd_ballot((condition_0))));

#line 18402
    return uint4(uint(_S200 & 4294967295ULL), uint(_S200 >> 32U), 0U, 0U);
}


#line 18825
uint _WaveCountBits_0(uint4 value_0, KernelContext_0 thread* kernelContext_4)
{

#line 18838
    switch((kernelContext_4->_S67 - 1U) / 32U)
    {
    case 0U:
    default:
        {

#line 18841
            return ((uint)popcount((value_0.x)));
        }
    case 1U:
        {

#line 18842
            return ((uint)popcount((value_0.x))) + ((uint)popcount((value_0.y)));
        }
    case 2U:
        {

#line 18843
            return ((uint)popcount((value_0.x))) + ((uint)popcount((value_0.y))) + ((uint)popcount((value_0.z)));
        }
    case 3U:
        {

#line 18844
            return ((uint)popcount((value_0.x))) + ((uint)popcount((value_0.y))) + ((uint)popcount((value_0.z))) + ((uint)popcount((value_0.w)));
        }
    }

#line 18844
}


#line 18416
uint WaveActiveCountBits_0(bool value_1, KernelContext_0 thread* kernelContext_5)
{

#line 18424
    uint4 _S201 = WaveActiveBallot_0(value_1);

#line 18424
    uint _S202 = _WaveCountBits_0(_S201, kernelContext_5);

#line 18424
    return _S202;
}


#line 37 "engine/render/shaders/veach/radiance_cache.slang"
void count_0(const RadianceCache_0 thread* c_20, uint index_0, bool event_0, KernelContext_0 thread* kernelContext_6)
{

#line 37
    uint _S203 = WaveActiveCountBits_0(event_0, kernelContext_6);

    bool _S204 = simd_is_first();

#line 39
    bool _S205;

#line 39
    if(_S204)
    {

#line 39
        _S205 = _S203 > 0U;

#line 39
    }
    else
    {

#line 39
        _S205 = false;

#line 39
    }

#line 39
    if(_S205)
    {

#line 40
        uint _S206 = atomic_fetch_add_explicit(((atomic_uint device*)(c_20->stats_0+index_0)), _S203, memory_order_relaxed);

#line 39
    }


    return;
}


#line 5 "build/release/apps/lab/generated/veach/materials_generated.slang"
SurfaceSample_0 evaluateSurface_0(uint kind_5, const MaterialContext_0 thread* c_21)
{

#line 6
    switch(kind_5)
    {
    case 1U:
        {

#line 6
            SurfaceSample_0 _S207 = evaluate_1(c_21);
            return _S207;
        }
    default:
        {

#line 7
            SurfaceSample_0 _S208 = evaluate_0(c_21);
            return _S208;
        }
    }

#line 8
}


#line 10 "engine/render/shaders/veach/lights.slang"
float emissive_selection_probability_0(KernelContext_0 thread* kernelContext_7)
{

#line 10
    float _S209;
    if(((&kernelContext_7->veach_scene_0)->emissive_count_1) == 0U)
    {

#line 11
        _S209 = 0.0f;

#line 11
    }
    else
    {

#line 11
        if(((&kernelContext_7->veach_scene_0)->light_count_1) == 0U)
        {

#line 11
            _S209 = 1.0f;

#line 11
        }
        else
        {

#line 11
            _S209 = 0.5f;

#line 11
        }

#line 11
    }

#line 11
    return _S209;
}


float emissive_light_pdf_0(const MaterialRecord_0 thread* m_1, float area_1, float distance_0, float cos_light_0, KernelContext_0 thread* kernelContext_8)
{

#line 15
    bool _S210;
    if(((&kernelContext_8->veach_scene_0)->emissive_total_weight_1) <= 0.0f)
    {

#line 16
        _S210 = true;

#line 16
    }
    else
    {

#line 16
        _S210 = cos_light_0 <= 0.0f;

#line 16
    }

#line 16
    if(_S210)
    {

#line 17
        return 0.0f;
    }

#line 17
    float _S211 = emissive_selection_probability_0(kernelContext_8);


    return _S211 * m_1->emissive_weight_0 / (&kernelContext_8->veach_scene_0)->emissive_total_weight_1 * distance_0 * distance_0 / cos_light_0;
}


#line 139 "engine/render/shaders/veach/brdf.slang"
float power_heuristic_0(float a_3, float b_8)
{

#line 140
    float _S212 = a_3 * a_3;

#line 140
    return _S212 / max(_S212 + b_8 * b_8, 9.99999968265522539e-21f);
}


#line 17 "engine/render/shaders/veach/diffuse_path.slang"
struct DiffuseVertex_0
{
    float3 position_6;
    float3 geometric_5;
    float3 normal_9;
    float3 albedo_0;
    float3 emission_3;
    bool lit_0;
};


DiffuseVertex_0 diffuse_vertex_0(const Trace_0 thread* t_6, float3 origin_2, float3 direction_4, float previous_pdf_0, KernelContext_0 thread* kernelContext_9)
{

#line 28
    TriangleHit_0 _S213 = surface_at_0(t_6->instance_1, t_6->primitive_2, t_6->bary_1, origin_2, kernelContext_9);

#line 28
    thread SurfaceInput_0 _S214 = _S213.surface_4;

#line 28
    thread MaterialRecord_0 _S215 = _S213.material_0;

#line 28
    MaterialContext_0 _S216 = MaterialContext_x24init_1(&_S214, &_S215);

#line 28
    thread MaterialContext_0 _S217 = _S216;

#line 28
    SurfaceSample_0 _S218 = evaluateSurface_0(_S213.material_0.kind_0, &_S217);



    thread DiffuseVertex_0 v_8;
    (&v_8)->position_6 = _S213.surface_4.position_0;
    (&v_8)->geometric_5 = _S213.surface_4.geometric_normal_0;
    (&v_8)->normal_9 = _S218.normal_1;
    (&v_8)->albedo_0 = _S218.base_color_1 * float3((1.0f - _S218.metallic_0)) ;
    float3 _S219 = - direction_4;

#line 37
    (&v_8)->lit_0 = (dot(_S218.normal_1, _S219)) > 0.0f;
    (&v_8)->emission_3 = float3(0.0f, 0.0f, 0.0f);

#line 38
    bool _S220;
    if(any((_S218.emission_0) > (float3(0.0f) )))
    {

#line 39
        if(_S213.front_0)
        {

#line 39
            _S220 = true;

#line 39
        }
        else
        {

#line 39
            _S220 = ((_S213.material_0.flags_0) & 64U) != 0U;

#line 39
        }

#line 39
    }
    else
    {

#line 39
        _S220 = false;

#line 39
    }

#line 39
    if(_S220)
    {
        if(previous_pdf_0 > 0.0f)
        {

#line 41
            _S220 = ((_S213.material_0.flags_0) & 128U) != 0U;

#line 41
        }
        else
        {

#line 41
            _S220 = false;

#line 41
        }

#line 41
        float weight_0;

#line 41
        if(_S220)
        {

#line 42
            float _S221 = dot((&v_8)->geometric_5, _S219);

#line 42
            thread MaterialRecord_0 _S222 = _S213.material_0;

#line 42
            float _S223 = emissive_light_pdf_0(&_S222, _S213.area_0, t_6->t_3, _S221, kernelContext_9);

#line 42
            weight_0 = power_heuristic_0(previous_pdf_0, _S223);

#line 41
        }
        else
        {

#line 41
            weight_0 = 1.0f;

#line 41
        }



        (&v_8)->emission_3 = _S218.emission_0 * float3(weight_0) ;

#line 39
    }

#line 47
    return v_8;
}


#line 136 "engine/render/shaders/veach/lights.slang"
uint pick_emissive_0(float u_0, KernelContext_0 thread* kernelContext_10)
{
    uint _S224 = (&kernelContext_10->veach_scene_0)->emissive_count_1 - 1U;

#line 138
    uint lo_0 = 0U;

#line 138
    uint hi_0 = _S224;
    for(;;)
    {

#line 139
        if(lo_0 < hi_0)
        {
        }
        else
        {

#line 139
            break;
        }

#line 140
        uint mid_0 = (lo_0 + hi_0) / 2U;
        if(((&kernelContext_10->veach_scene_0)->emissive_1[mid_0].cdf_0) < u_0)
        {

#line 141
            lo_0 = mid_0 + 1U;

#line 141
        }
        else
        {

#line 141
            hi_0 = mid_0;

#line 141
        }

#line 139
    }

#line 147
    return lo_0;
}


#line 23
float3 emission_of_0(const TriangleHit_0 thread* hit_2)
{

#line 23
    bool _S225;
    if(!hit_2->front_0)
    {

#line 24
        _S225 = (((&hit_2->material_0)->flags_0) & 64U) == 0U;

#line 24
    }
    else
    {

#line 24
        _S225 = false;

#line 24
    }

#line 24
    if(_S225)
    {

#line 25
        return float3(0.0f, 0.0f, 0.0f);
    }

#line 25
    MaterialContext_0 _S226 = MaterialContext_x24init_1(&hit_2->surface_4, &hit_2->material_0);

#line 25
    thread MaterialContext_0 _S227 = _S226;

#line 25
    SurfaceSample_0 _S228 = evaluateSurface_0((&hit_2->material_0)->kind_0, &_S227);


    return _S228.emission_0;
}


#line 81 "engine/render/shaders/veach/brdf.slang"
float3 lambert_eval_0(float3 albedo_1, float3 l_2, float thread* pdf_2)
{

#line 82
    float _S229 = max(l_2.z, 0.0f) / 3.1415927410125732f;

#line 82
    *pdf_2 = _S229;
    return albedo_1 * float3(_S229) ;
}


#line 51
float ggx_d_0(float n_dot_h_0, float a2_0)
{

#line 52
    float d_1 = n_dot_h_0 * n_dot_h_0 * (a2_0 - 1.0f) + 1.0f;
    return a2_0 / (3.1415927410125732f * d_1 * d_1);
}

float smith_g1_0(float n_dot_x_0, float a2_1)
{

#line 57
    return 2.0f * n_dot_x_0 / (n_dot_x_0 + sqrt(a2_1 + (1.0f - a2_1) * n_dot_x_0 * n_dot_x_0));
}


float3 brdf_eval_0(const Brdf_0 thread* b_9, float3 v_9, float3 l_3, float thread* pdf_3)
{

#line 62
    *pdf_3 = 0.0f;
    float _S230 = l_3.z;

#line 63
    bool _S231;

#line 63
    if(_S230 <= 0.0f)
    {

#line 63
        _S231 = true;

#line 63
    }
    else
    {

#line 63
        _S231 = (v_9.z) <= 0.0f;

#line 63
    }

#line 63
    if(_S231)
    {

#line 64
        return float3(0.0f, 0.0f, 0.0f);
    }
    float3 h_2 = normalize(v_9 + l_3);
    float a2_2 = b_9->alpha_1 * b_9->alpha_1;
    float d_2 = ggx_d_0(h_2.z, a2_2);
    float _S232 = v_9.z;

#line 69
    float _S233 = 1.0f - a2_2;

#line 69
    float3 _S234 = float3(1.0f) ;
    float3 f_1 = b_9->f0_0 + (_S234 - b_9->f0_0) * float3(pow(1.0f - saturate(dot(v_9, h_2)), 5.0f)) ;
    float3 specular_1 = float3((d_2 * (0.5f / (_S230 * sqrt(_S232 * _S232 * _S233 + a2_2) + _S232 * sqrt(_S230 * _S230 * _S233 + a2_2)))))  * f_1;
    float3 diffuse_3 = (_S234 - f_1) * b_9->diffuse_1 / float3(3.1415927410125732f) ;


    *pdf_3 = b_9->specular_probability_0 * (smith_g1_0(_S232, a2_2) * d_2 / (4.0f * _S232)) + (1.0f - b_9->specular_probability_0) * (_S230 / 3.1415927410125732f);
    return (specular_1 + diffuse_3) * float3(_S230) ;
}


#line 32 "engine/render/shaders/veach/lights.slang"
float3 punctual_irradiance_0(const LightRecord_natural_0 thread* light_0, float3 p_7, float3 thread* l_4, float thread* distance_1)
{

#line 33
    float3 irradiance_1 = (float4(light_0->radiance_0) ).xyz;

#line 33
    uint _S235 = light_0->kind_1;

#line 33
    float3 irradiance_2;
    if((light_0->kind_1) == 0U)
    {

#line 35
        *l_4 = - (float4(light_0->direction_0) ).xyz;
        *distance_1 = 1.00000001504746622e+30f;

#line 36
        irradiance_2 = irradiance_1;

#line 34
    }
    else
    {

        float3 to_light_0 = (float4(light_0->position_1) ).xyz - p_7;
        float _S236 = length(to_light_0);

#line 39
        *distance_1 = _S236;
        *l_4 = to_light_0 / float3(_S236) ;
        float3 irradiance_3 = irradiance_1 / float3((*distance_1 * *distance_1)) ;
        if(_S235 == 2U)
        {
            float falloff_0 = saturate((dot((float4(light_0->direction_0) ).xyz, - *l_4) - light_0->cos_outer_0) / max(light_0->cos_inner_0 - light_0->cos_outer_0, 9.9999997473787516e-05f));

#line 44
            irradiance_2 = irradiance_3 * float3((falloff_0 * falloff_0)) ;

#line 42
        }
        else
        {

#line 42
            irradiance_2 = irradiance_3;

#line 42
        }

#line 34
    }

#line 48
    return irradiance_2;
}


#line 58
void sample_light_extent_0(const LightRecord_natural_0 thread* light_1, float2 u_1, float3 thread* point_1, float3 thread* normal_10)
{

#line 59
    if((light_1->kind_1) == 0U)
    {

#line 60
        float cos_theta_0 = 1.0f - u_1.x * (1.0f - cos(light_1->size_0));
        float sin_theta_0 = sqrt(max(1.0f - cos_theta_0 * cos_theta_0, 0.0f));

        float _S237 = 6.2831854820251465f * u_1.y;

#line 63
        float3 _S238 = float3(sin_theta_0 * cos(_S237), sin_theta_0 * sin(_S237), cos_theta_0);

#line 63
        thread Frame_0 _S239 = frame_of_0(- (float4(light_1->direction_0) ).xyz);

#line 63
        float3 _S240 = Frame_to_world_0(&_S239, _S238);

#line 63
        *normal_10 = _S240;
        *point_1 = float3(0.0f, 0.0f, 0.0f);
        return;
    }
    float z_0 = 1.0f - 2.0f * u_1.x;
    float r_5 = sqrt(max(1.0f - z_0 * z_0, 0.0f));
    float _S241 = 6.2831854820251465f * u_1.y;

#line 69
    float3 _S242 = float3(r_5 * cos(_S241), r_5 * sin(_S241), z_0);

#line 69
    *normal_10 = _S242;
    *point_1 = (float4(light_1->position_1) ).xyz + float3(light_1->size_0)  * _S242;
    return;
}

float light_extent_pdf_0(const LightRecord_natural_0 thread* light_2)
{

#line 75
    if((light_2->kind_1) == 0U)
    {

#line 76
        return 1.0f / (6.2831854820251465f * max(1.0f - cos(light_2->size_0), 9.999999960041972e-13f));
    }
    return 1.0f / (12.566370964050293f * light_2->size_0 * light_2->size_0);
}



float3 light_extent_incoming_0(const LightRecord_natural_0 thread* light_3, float3 point_2, float3 normal_11, float3 p_8, float3 thread* l_5, float thread* distance_2)
{

#line 83
    uint _S243 = light_3->kind_1;
    if((light_3->kind_1) == 0U)
    {

#line 85
        *l_5 = normal_11;
        *distance_2 = 1.00000001504746622e+30f;
        float3 _S244 = (float4(light_3->radiance_0) ).xyz;

#line 87
        float _S245 = light_extent_pdf_0(light_3);

#line 87
        return _S244 * float3(_S245) ;
    }
    float3 to_light_1 = point_2 - p_8;
    float _S246 = length(to_light_1);

#line 90
    *distance_2 = _S246;

#line 90
    float3 _S247;
    if(_S246 > 9.99999997475242708e-07f)
    {

#line 91
        _S247 = to_light_1 / float3(*distance_2) ;

#line 91
    }
    else
    {

#line 91
        _S247 = float3(0.0f, 0.0f, 1.0f);

#line 91
    }

#line 91
    *l_5 = _S247;
    float cos_light_1 = dot(normal_11, - _S247);

#line 92
    bool _S248;
    if((*distance_2) <= 9.99999997475242708e-07f)
    {

#line 93
        _S248 = true;

#line 93
    }
    else
    {

#line 93
        _S248 = cos_light_1 <= 0.0f;

#line 93
    }

#line 93
    if(_S248)
    {

#line 94
        return float3(0.0f, 0.0f, 0.0f);
    }

#line 94
    float falloff_1;


    if(_S243 == 2U)
    {
        float falloff_2 = saturate((dot((float4(light_3->direction_0) ).xyz, normalize(p_8 - (float4(light_3->position_1) ).xyz)) - light_3->cos_outer_0) / max(light_3->cos_inner_0 - light_3->cos_outer_0, 9.9999997473787516e-05f));

#line 99
        falloff_1 = falloff_2 * falloff_2;

#line 97
    }
    else
    {

#line 97
        falloff_1 = 1.0f;

#line 97
    }

#line 103
    return (float4(light_3->radiance_0) ).xyz * float3(falloff_1)  / float3((3.1415927410125732f * light_3->size_0 * light_3->size_0))  * float3(cos_light_1)  / float3((*distance_2 * *distance_2)) ;
}



float3 punctual_irradiance_sample_0(const LightRecord_natural_0 thread* light_4, float3 p_9, Rng_0 thread* rng_3, float3 thread* l_6, float thread* distance_3)
{

#line 109
    if((light_4->size_0) <= 0.0f)
    {

#line 109
        float3 _S249 = punctual_irradiance_0(light_4, p_9, l_6, distance_3);
        return _S249;
    }


    float _S250 = Rng_next_0(rng_3);

#line 114
    float _S251 = Rng_next_0(rng_3);

#line 112
    thread float3 point_3;
    thread float3 normal_12;

#line 113
    sample_light_extent_0(light_4, float2(_S250, _S251), &point_3, &normal_12);

#line 113
    float3 _S252 = light_extent_incoming_0(light_4, point_3, normal_12, p_9, l_6, distance_3);

#line 113
    float _S253 = light_extent_pdf_0(light_4);

    return _S252 / float3(_S253) ;
}


float3 sample_punctual_0(float3 p_10, const Frame_0 thread* frame_5, const Brdf_0 thread* b_10, float3 v_local_0, float p_punctual_0, Rng_0 thread* rng_4, float3 thread* l_7, float thread* distance_4, bool lambert_0, KernelContext_0 thread* kernelContext_11)
{
    *l_7 = float3(0.0f, 0.0f, 1.0f);
    *distance_4 = 0.0f;
    if(((&kernelContext_11->veach_scene_0)->light_count_1) == 0U)
    {

#line 124
        return float3(0.0f, 0.0f, 0.0f);
    }
    float _S254 = Rng_next_0(rng_4);
    float select_0 = p_punctual_0 / float((&kernelContext_11->veach_scene_0)->light_count_1);

#line 127
    thread LightRecord_natural_0 _S255 = (&kernelContext_11->veach_scene_0)->lights_1[min(uint(_S254 * float((&kernelContext_11->veach_scene_0)->light_count_1)), (&kernelContext_11->veach_scene_0)->light_count_1 - 1U)];

#line 127
    float3 _S256 = punctual_irradiance_sample_0(&_S255, p_10, rng_4, l_7, distance_4);

    thread float bsdf_pdf_0;

#line 129
    float3 f_2;
    if(lambert_0)
    {

#line 130
        float3 _S257 = b_10->diffuse_1;

#line 130
        float3 _S258 = Frame_to_local_0(frame_5, *l_7);

#line 130
        float3 _S259 = lambert_eval_0(_S257, _S258, &bsdf_pdf_0);

#line 130
        f_2 = _S259;

#line 130
    }
    else
    {

#line 130
        float3 _S260 = Frame_to_local_0(frame_5, *l_7);

#line 130
        float3 _S261 = brdf_eval_0(b_10, v_local_0, _S260, &bsdf_pdf_0);

#line 130
        f_2 = _S261;

#line 130
    }

    return _S256 * f_2 / float3(select_0) ;
}


#line 328 "engine/render/shaders/veach/scene.slang"
bool visible_0(float3 from_0, float3 to_direction_0, float distance_5, Rng_0 thread* rng_5, KernelContext_0 thread* kernelContext_12)
{

#line 329
    bool skip_alpha_0 = (((&kernelContext_12->veach_scene_0)->debug_skip_1) & 2U) != 0U;
    thread RayDesc ray_1;
    (&ray_1)->Origin = from_0;
    (&ray_1)->Direction = to_direction_0;
    (&ray_1)->TMin = 0.0f;
    (&ray_1)->TMax = distance_5;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> opaque_0;
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> _S262 = getDescriptorFromHandle_0((&kernelContext_12->veach_scene_0)->tlas_1);

#line 336
    uint _S263;
    if(skip_alpha_0)
    {

#line 337
        _S263 = 255U;

#line 337
    }
    else
    {

#line 337
        _S263 = 1U;

#line 337
    }

#line 336
    (&opaque_0)->reset({ (ray_1.Origin), (ray_1.Direction), (ray_1.TMin), (ray_1.TMax) }, (_S262), (_S263), _slang_ray_flags_to_intersection_params((5U)));

    for(;;)
    {

#line 338
        bool _S264 = ((&opaque_0)->next());

#line 338
        if(_S264)
        {
        }
        else
        {

#line 338
            break;
        }

#line 338
    }

    uint _S265 = (_slang_committed_intersection_type_to_uint((&opaque_0)->get_committed_intersection_type()));

#line 340
    if(_S265 == 1U)
    {

#line 341
        return false;
    }

#line 341
    bool _S266;

    if(skip_alpha_0)
    {

#line 343
        _S266 = true;

#line 343
    }
    else
    {

#line 343
        _S266 = (((&kernelContext_12->veach_scene_0)->flags_3) & 1U) == 0U;

#line 343
    }

#line 343
    if(_S266)
    {

#line 344
        return true;
    }

    metal::raytracing::acceleration_structure<metal::raytracing::instancing> _S267 = getDescriptorFromHandle_0((&kernelContext_12->veach_scene_0)->tlas_1);

#line 346
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> alpha_2;
    (&alpha_2)->reset({ (ray_1.Origin), (ray_1.Direction), (ray_1.TMin), (ray_1.TMax) }, (_S267), (2U), _slang_ray_flags_to_intersection_params((4U)));
    for(;;)
    {

#line 348
        bool _S268 = ((&alpha_2)->next());

#line 348
        if(_S268)
        {
        }
        else
        {

#line 348
            break;
        }

#line 349
        uint _S269 = (
                        _slang_candidate_intersection_type_to_uint((&alpha_2)->get_candidate_intersection_type())
                    );

#line 349
        if(_S269 == 0U)
        {

#line 350
            uint _S270 = ((&alpha_2)->get_candidate_instance_id());

#line 350
            uint _S271 = ((&alpha_2)->get_candidate_primitive_id());
            float2 _S272 = ((&alpha_2)->get_candidate_triangle_barycentric_coord());

#line 351
            bool _S273 = covered_0(_S270, _S271, _S272, rng_5, kernelContext_12);

#line 351
            _S266 = _S273;

#line 349
        }
        else
        {

#line 349
            _S266 = false;

#line 349
        }

#line 349
        if(_S266)
        {

            (&alpha_2)->commit_triangle_intersection();

#line 349
        }

#line 348
    }

#line 355
    uint _S274 = (_slang_committed_intersection_type_to_uint((&alpha_2)->get_committed_intersection_type()));

#line 355
    return _S274 != 1U;
}


#line 362
bool light_visible_0(float3 p_11, float3 g_1, float3 l_8, float distance_6, Rng_0 thread* rng_6, KernelContext_0 thread* kernelContext_13)
{

#line 363
    if((((&kernelContext_13->veach_scene_0)->debug_skip_1) & 1U) != 0U)
    {

#line 364
        return true;
    }
    float3 from_1 = offset_origin_0(p_11, g_1, l_8);
    if(distance_6 >= 1.00000001504746622e+29f)
    {

#line 367
        bool _S275 = visible_0(from_1, l_8, distance_6, rng_6, kernelContext_13);
        return _S275;
    }
    float3 to_light_2 = p_11 + l_8 * float3(distance_6)  - from_1;
    float length_to_light_0 = length(to_light_2);

#line 371
    bool _S276 = visible_0(from_1, to_light_2 / float3(length_to_light_0) , length_to_light_0 * 0.99900001287460327f, rng_6, kernelContext_13);
    return _S276;
}


#line 154 "engine/render/shaders/veach/lights.slang"
float3 sample_light_0(float3 p_12, float3 geometric_6, const Frame_0 thread* frame_6, const Brdf_0 thread* b_11, float3 v_local_1, Rng_0 thread* rng_7, bool lambert_1, bool mis_0, KernelContext_0 thread* kernelContext_14)
{
    float pick_0 = Rng_next_0(rng_7);

#line 156
    float _S277 = emissive_selection_probability_0(kernelContext_14);


    thread float3 l_9 = float3(0.0f, 0.0f, 1.0f);
    thread float distance_7 = 0.0f;

#line 160
    bool _S278;

#line 160
    float3 contribution_0;
    if(pick_0 < _S277)
    {
        EmissiveTriangle_natural_0 device* _S279 = (&kernelContext_14->veach_scene_0)->emissive_1;

#line 163
        float _S280 = Rng_next_0(rng_7);

#line 163
        uint _S281 = pick_emissive_0(_S280, kernelContext_14);

#line 163
        EmissiveTriangle_natural_0 tri_0 = _S279[_S281];
        float _S282 = Rng_next_0(rng_7);

#line 164
        float su_0 = sqrt(_S282);
        float _S283 = 1.0f - su_0;

#line 165
        float _S284 = Rng_next_0(rng_7);

#line 165
        TriangleHit_0 _S285 = surface_at_0(tri_0.instance_0, tri_0.primitive_0, float2(_S283, _S284 * su_0), p_12, kernelContext_14);

        float3 to_light_3 = _S285.surface_4.position_0 - p_12;
        float _S286 = length(to_light_3);

#line 168
        distance_7 = _S286;
        float3 _S287 = to_light_3 / float3(_S286) ;

#line 169
        l_9 = _S287;
        float cos_light_2 = dot(_S285.surface_4.geometric_normal_0, - _S287);

#line 170
        thread TriangleHit_0 _S288 = _S285;

#line 170
        float3 _S289 = emission_of_0(&_S288);

#line 170
        thread MaterialRecord_0 _S290 = _S285.material_0;

#line 170
        float _S291 = emissive_light_pdf_0(&_S290, _S285.area_0, _S286, cos_light_2, kernelContext_14);


        if(_S291 <= 0.0f)
        {

#line 173
            _S278 = true;

#line 173
        }
        else
        {

#line 173
            _S278 = all(_S289 <= (float3(0.0f) ));

#line 173
        }

#line 173
        if(_S278)
        {

#line 174
            return float3(0.0f, 0.0f, 0.0f);
        }
        thread float bsdf_pdf_1;
        if(lambert_1)
        {

#line 177
            float3 _S292 = b_11->diffuse_1;

#line 177
            float3 _S293 = Frame_to_local_0(frame_6, l_9);

#line 177
            float3 _S294 = lambert_eval_0(_S292, _S293, &bsdf_pdf_1);

#line 177
            contribution_0 = _S294;

#line 177
        }
        else
        {

#line 177
            float3 _S295 = Frame_to_local_0(frame_6, l_9);

#line 177
            float3 _S296 = brdf_eval_0(b_11, v_local_1, _S295, &bsdf_pdf_1);

#line 177
            contribution_0 = _S296;

#line 177
        }

        float3 _S297 = _S289 * contribution_0;

#line 179
        float _S298;

#line 179
        if(mis_0)
        {

#line 179
            _S298 = power_heuristic_0(_S291, bsdf_pdf_1);

#line 179
        }
        else
        {

#line 179
            _S298 = 1.0f;

#line 179
        }

#line 179
        contribution_0 = _S297 * float3(_S298)  / float3(_S291) ;

#line 161
    }
    else
    {

#line 161
        float3 _S299 = sample_punctual_0(p_12, frame_6, b_11, v_local_1, 1.0f - _S277, rng_7, &l_9, &distance_7, lambert_1, kernelContext_14);

#line 161
        contribution_0 = _S299;

#line 161
    }

#line 183
    if(all(contribution_0 <= (float3(0.0f) )))
    {

#line 183
        _S278 = true;

#line 183
    }
    else
    {

#line 183
        bool _S300 = light_visible_0(p_12, geometric_6, l_9, distance_7, rng_7, kernelContext_14);

#line 183
        _S278 = !_S300;

#line 183
    }

#line 183
    if(_S278)
    {

#line 184
        return float3(0.0f, 0.0f, 0.0f);
    }
    return contribution_0;
}


#line 51 "engine/render/shaders/veach/diffuse_path.slang"
float3 diffuse_light_0(const DiffuseVertex_0 thread* v_10, Rng_0 thread* rng_8, KernelContext_0 thread* kernelContext_15)
{

#line 52
    thread Brdf_0 b_12;
    (&b_12)->diffuse_1 = v_10->albedo_0;
    (&b_12)->f0_0 = float3(0.0f, 0.0f, 0.0f);
    (&b_12)->alpha_1 = 1.0f;
    (&b_12)->specular_probability_0 = 0.0f;
    float3 _S301 = float3(0.0f, 0.0f, 1.0f);

#line 57
    thread Frame_0 _S302 = frame_of_0(v_10->normal_9);

#line 57
    thread Brdf_0 _S303 = b_12;

#line 57
    float3 _S304 = sample_light_0(v_10->position_6, v_10->geometric_5, &_S302, &_S303, _S301, rng_8, true, true, kernelContext_15);

#line 57
    return _S304;
}


#line 123 "engine/render/shaders/veach/radiance_cache.slang"
void cache_add_at_0(const RadianceCache_0 thread* c_22, const CacheKey_0 thread* k_2, uint3 units_0, KernelContext_0 thread* kernelContext_16)
{

#line 123
    bool added_0 = false;

#line 123
    bool claimed_0 = false;

#line 123
    uint i_1 = 0U;


    for(;;)
    {

#line 126
        bool _S305;

#line 126
        if(i_1 < 8U)
        {

#line 126
            _S305 = !added_0;

#line 126
        }
        else
        {

#line 126
            _S305 = false;

#line 126
        }

#line 126
        if(_S305)
        {
        }
        else
        {

#line 126
            break;
        }

#line 127
        uint slot_3 = (k_2->slot_1 + i_1) & (c_22->capacity_0 - 1U);

#line 127
        uint _S306 = k_2->checksum_0;

        uint previous_0;
        {
        uint _metal_cas_comparand = 0U;
        atomic_compare_exchange_weak_explicit(((atomic_uint device*)(c_22->keys_0+slot_3)), &_metal_cas_comparand, k_2->checksum_0, memory_order_relaxed, memory_order_relaxed);
        previous_0 = _metal_cas_comparand;
        }

#line 130
        bool _S307 = previous_0 == 0U;

#line 130
        bool _S308;

#line 130
        if(_S307)
        {

#line 130
            _S308 = true;

#line 130
        }
        else
        {

#line 130
            _S308 = previous_0 == _S306;

#line 130
        }

#line 130
        if(_S308)
        {

#line 130
            uint device* _S309 = c_22->accum_0;
            uint _S310 = slot_3 * 4U;

#line 131
            uint _S311 = atomic_fetch_add_explicit(((atomic_uint device*)(c_22->accum_0+_S310)), units_0.x, memory_order_relaxed);
            uint _S312 = atomic_fetch_add_explicit(((atomic_uint device*)(_S309+(_S310 + 1U))), units_0.y, memory_order_relaxed);
            uint _S313 = atomic_fetch_add_explicit(((atomic_uint device*)(_S309+(_S310 + 2U))), units_0.z, memory_order_relaxed);
            uint _S314 = atomic_fetch_add_explicit(((atomic_uint device*)(_S309+(_S310 + 3U))), 1U, memory_order_relaxed);

#line 134
            added_0 = true;

#line 134
            claimed_0 = _S307;

#line 130
        }

#line 126
        i_1 = i_1 + 1U;

#line 126
    }

#line 126
    count_0(c_22, 3U, true, kernelContext_16);

#line 126
    count_0(c_22, 4U, claimed_0, kernelContext_16);

#line 126
    count_0(c_22, 5U, !added_0, kernelContext_16);

#line 142
    return;
}


void cache_add_0(const RadianceCache_0 thread* c_23, float3 p_13, float3 n_9, float3 radiance_5, uint kind_6, KernelContext_0 thread* kernelContext_17)
{

#line 147
    uint3 _S315 = uint3(min(max(radiance_5, float3(0.0f) ) * float3(c_23->scale_0) , float3(4.0e+06f) ));

#line 147
    uint _S316 = cache_level_0(c_23, p_13);

#line 147
    uint l_10 = _S316;

    for(;;)
    {

#line 149
        if(l_10 <= (min(_S316 + 2U, 15U)))
        {
        }
        else
        {

#line 149
            break;
        }

#line 149
        CacheKey_0 _S317 = cache_key_0(c_23, p_13, n_9, l_10, kind_6);

#line 149
        thread CacheKey_0 _S318 = _S317;

#line 149
        cache_add_at_0(c_23, &_S318, _S315, kernelContext_17);

#line 149
        l_10 = l_10 + 1U;

#line 149
    }


    return;
}


#line 253 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
float3 gi_direction_0(const GiReservoir_0 thread* r_6, float3 p_14)
{

#line 253
    float3 _S319;
    if(r_6->sky_1)
    {

#line 254
        _S319 = r_6->point_0;

#line 254
    }
    else
    {

#line 254
        _S319 = normalize(r_6->point_0 - p_14);

#line 254
    }

#line 254
    return _S319;
}



float gi_geometry_0(const GiReservoir_0 thread* r_7, float3 p_15, float3 n_10)
{

#line 259
    float3 _S320 = gi_direction_0(r_7, p_15);
    return max(dot(n_10, _S320), 0.0f);
}


#line 269
float gi_source_pdf_0(const GiReservoir_0 thread* r_8, float3 p_16, float3 n_11)
{

#line 269
    float _S321 = gi_geometry_0(r_8, p_16, n_11);
    return _S321 / 3.1415927410125732f;
}


#line 264
float gi_target_0(const GiReservoir_0 thread* r_9, float3 p_17, float3 n_12)
{

#line 265
    float _S322 = luminance_0(r_9->radiance_2);

#line 265
    float _S323 = gi_geometry_0(r_9, p_17, n_12);

#line 265
    return _S322 * _S323;
}


#line 330
void gi_finish_0(GiReservoir_0 thread* r_10, float3 p_18, float3 n_13)
{

#line 330
    thread GiReservoir_0 _S324 = *r_10;

#line 330
    float _S325 = gi_target_0(&_S324, p_18, n_13);

#line 330
    bool _S326;

    if(_S325 > 0.0f)
    {

#line 332
        _S326 = (r_10->M_0) > 0.0f;

#line 332
    }
    else
    {

#line 332
        _S326 = false;

#line 332
    }

#line 332
    float _S327;

#line 332
    if(_S326)
    {

#line 332
        _S327 = r_10->w_sum_0 / (r_10->M_0 * _S325);

#line 332
    }
    else
    {

#line 332
        _S327 = 0.0f;

#line 332
    }

#line 332
    r_10->w_sum_0 = _S327;
    return;
}


#line 333
void use_scene_0(RealtimeFrame_natural_0 device* _S328, int _S329, KernelContext_0 thread* kernelContext_18)
{

#line 333
    RealtimeFrame_natural_0 device* _S330 = _S328+_S329;

#line 173 "engine/render/shaders/veach/realtime.slang"
    (&kernelContext_18->veach_scene_0)->tlas_1 = _S330->tlas_0;
    (&kernelContext_18->veach_scene_0)->instances_1 = _S330->instances_0;
    (&kernelContext_18->veach_scene_0)->materials_1 = _S330->materials_0;
    (&kernelContext_18->veach_scene_0)->lights_1 = _S330->lights_0;
    (&kernelContext_18->veach_scene_0)->emissive_1 = _S330->emissive_0;
    (&kernelContext_18->veach_scene_0)->light_count_1 = _S330->light_count_0;
    (&kernelContext_18->veach_scene_0)->emissive_count_1 = _S330->emissive_count_0;
    (&kernelContext_18->veach_scene_0)->emissive_total_weight_1 = _S330->emissive_total_weight_0;
    (&kernelContext_18->veach_scene_0)->debug_skip_1 = _S330->debug_skip_0;
    (&kernelContext_18->veach_scene_0)->flags_3 = _S330->scene_flags_0;
    return;
}


#line 2816 "core.meta.slang"
GSurface_0 load_lit_surface_0(RealtimeFrame_natural_0 device* _S331, int _S332, int2 _S333)
{

#line 2816
    RealtimeFrame_natural_0 device* _S334 = _S331+_S332;

#line 227 "engine/render/shaders/veach/realtime.slang"
    int3 _S335 = int3(_S333, int(0));

#line 227
    float4 p_19 = ((_S334->position_2).read(vec<uint,2>(((_S335)).xy), uint(((_S335)).z)));

#line 226
    thread GSurface_0 s_7;

    float _S336 = p_19.w;

#line 228
    (&s_7)->valid_0 = _S336 > 0.0f;
    (&s_7)->position_3 = p_19.xyz;
    (&s_7)->depth_0 = _S336;
    float4 n_14 = ((_S334->normal_2).read(vec<uint,2>(((_S335)).xy), uint(((_S335)).z)));
    (&s_7)->normal_3 = n_14.xyz;
    (&s_7)->roughness_2 = n_14.w;
    float4 g_2 = ((_S334->geometric_0).read(vec<uint,2>(((_S335)).xy), uint(((_S335)).z)));
    (&s_7)->geometric_1 = g_2.xyz;
    (&s_7)->metallic_1 = g_2.w;
    (&s_7)->diffuse_2 = ((_S334->diffuse_0).read(vec<uint,2>(((_S335)).xy), uint(((_S335)).z))).xyz;
    (&s_7)->f0_1 = ((_S334->specular_0).read(vec<uint,2>(((_S335)).xy), uint(((_S335)).z))).xyz;
    (&s_7)->emission_2 = float3(0.0f, 0.0f, 0.0f);
    return s_7;
}


#line 240
RadianceCache_0 cache_of_0(RealtimeFrame_natural_0 device* _S337, int _S338)
{

#line 240
    RealtimeFrame_natural_0 device* _S339 = _S337+_S338;

#line 42 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
    thread RadianceCache_0 c_24;
    (&c_24)->keys_0 = _S339->cache_keys_0;
    (&c_24)->accum_0 = _S339->cache_accum_0;
    (&c_24)->radiance_3 = _S339->cache_radiance_0;
    (&c_24)->camera_2 = (float4(_S339->camera_position_0) ).xyz;
    (&c_24)->capacity_0 = _S339->cache_capacity_0;
    (&c_24)->scale_0 = _S339->cache_scale_0;
    (&c_24)->cell_size_0 = _S339->cache_cell_size_0;
    (&c_24)->cell_scale_0 = _S339->cache_cell_scale_0;
    (&c_24)->max_frames_0 = _S339->cache_max_frames_0;
    (&c_24)->stats_0 = _S339->counters_0;
    return c_24;
}


#line 53
float2 sky_view_uv_0(SkyFrame_natural_0 device* _S340, int _S341, float3 _S342)
{

#line 53
    SkyFrame_natural_0 device* _S343 = _S340+_S341;

#line 138 "engine/render/shaders/veach/atmosphere.slang"
    float _S344 = (float4(_S343->atmosphere_0.planet_0) ).x;

#line 138
    float r_11 = _S344 + (float4(_S343->camera_0) ).x;


    float horizon_elevation_0 = 1.5707963705062866f - acos(clamp(- sqrt(max(r_11 * r_11 - _S344 * _S344, 0.0f)) / r_11, -1.0f, 1.0f));
    float elevation_0 = 1.5707963705062866f - acos(clamp(_S342.y, -1.0f, 1.0f));

#line 142
    float v_11;

    if(elevation_0 >= horizon_elevation_0)
    {

#line 144
        v_11 = 0.5f + 0.5f * sqrt(saturate((elevation_0 - horizon_elevation_0) / (1.5707963705062866f - horizon_elevation_0)));

#line 144
    }
    else
    {

#line 144
        v_11 = 0.5f - 0.5f * sqrt(saturate((horizon_elevation_0 - elevation_0) / (horizon_elevation_0 + 1.5707963705062866f)));

#line 144
    }

#line 144
    float4 _S345 = float4(_S343->sun_0) ;

#line 153
    float delta_0 = atan2(_S342.z, _S342.x) - atan2(_S345.z, _S345.x);

    return float2(0.5f + (delta_0 - 6.2831854820251465f * floor((delta_0 + 3.1415927410125732f) / 6.2831854820251465f)) / 6.2831854820251465f, v_11);
}


#line 155
float3 sample_transmittance_0(SkyFrame_natural_0 device* _S346, int _S347, float _S348, float _S349)
{

#line 155
    SkyFrame_natural_0 device* _S350 = _S346+_S347;

#line 131
    texture2d<float, access::sample> _S351 = _S350->transmittance_0;

#line 131
    sampler _S352 = _S350->linear_clamp_0;

#line 131
    thread AtmosphereParams_natural_0 _S353 = _S350->atmosphere_0;

#line 131
    float2 _S354 = transmittance_uv_0(&_S353, _S348, _S349);

#line 131
    return ((_S351).sample((_S352), (_S354), level((0.0f)))).xyz;
}


#line 131
float3 sky_radiance_0(SkyFrame_natural_0 device* _S355, int _S356, float3 _S357, bool _S358)
{

#line 131
    SkyFrame_natural_0 device* _S359 = _S355+_S356;

#line 131
    uint _S360 = _S359->mode_0;

#line 182
    if((_S359->mode_0) == 0U)
    {

#line 183
        return float3(0.0f, 0.0f, 0.0f);
    }
    if(_S360 == 1U)
    {

#line 186
        return (float4(_S359->constant_0) ).xyz;
    }
    texture2d<float, access::sample> _S361 = _S359->sky_view_0;

#line 188
    sampler _S362 = _S359->linear_clamp_0;

#line 188
    float2 _S363 = sky_view_uv_0(_S355, _S356, _S357);

#line 188
    float3 l_11 = ((_S361).sample((_S362), (_S363), level((0.0f)))).xyz;

#line 188
    bool _S364;
    if(_S358)
    {

#line 189
        _S364 = (dot(_S357, (float4(_S359->sun_0) ).xyz)) > (cos(0.0046250000596046448f));

#line 189
    }
    else
    {

#line 189
        _S364 = false;

#line 189
    }

#line 189
    float3 l_12;

#line 189
    if(_S364)
    {

#line 190
        float _S365 = (float4(_S359->atmosphere_0.planet_0) ).x;

#line 190
        float r_12 = _S365 + (float4(_S359->camera_0) ).x;
        float solid_angle_0 = 6.2831854820251465f * (1.0f - cos(0.0046250000596046448f));

        if((ray_sphere_0(float3(0.0f, r_12, 0.0f), _S357, _S365)) < 0.0f)
        {

#line 193
            float3 _S366 = sample_transmittance_0(_S355, _S356, r_12, _S357.y);

#line 193
            l_12 = l_11 + _S366 / float3(solid_angle_0) ;

#line 193
        }
        else
        {

#line 193
            l_12 = l_11;

#line 193
        }

#line 189
    }
    else
    {

#line 189
        l_12 = l_11;

#line 189
    }

#line 197
    return l_12 * float3((float4(_S359->sun_0) ).w) ;
}


#line 350 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/a/candela_variant.slang"
[[kernel]] void gather(uint3 id_0 [[thread_position_in_grid]], uint _builtinWaveLaneCount_0 [[threads_per_simdgroup]], RealtimeParams_0 constant* realtime_params_1 [[buffer(0)]])
{

#line 350
    thread KernelContext_0 kernelContext_19;

#line 350
    (&kernelContext_19)->realtime_params_0 = realtime_params_1;

#line 350
    (&kernelContext_19)->_S67 = _builtinWaveLaneCount_0;
    RealtimeFrame_natural_0 device* _S367 = realtime_params_1->frame_4;

#line 351
    RealtimeFrame_natural_0 device* _S368 = realtime_params_1->frame_4+int(0);
    uint2 _S369 = id_0.xy;

#line 352
    uint2 _S370 = uint2(_S368->render_size_0) ;

#line 352
    if(any(_S369 >= _S370))
    {

#line 353
        return;
    }

#line 353
    use_scene_0(_S367, int(0), &kernelContext_19);


    uint _S371 = id_0.y;

#line 356
    uint _S372 = id_0.x;

#line 356
    uint index_1 = _S371 * _S370.x + _S372;

#line 356
    GSurface_0 _S373 = load_lit_surface_0(_S367, int(0), int2(_S369));

    float3 _S374 = (float4(_S368->camera_position_0) ).xyz;

#line 358
    thread GSurface_0 _S375 = _S373;

#line 358
    Domain_0 _S376 = domain_of_0(&_S375, _S374);
    if(!_S376.valid_1)
    {

#line 359
        PackedGiReservoir_natural_0 device* _S377 = _S368->gi_temporal_out_0+index_1;

#line 359
        thread GiReservoir_0 _S378 = empty_gi_0();

#line 359
        PackedGiReservoir_0 _S379 = pack_gi_0(&_S378);

#line 359
        _S377->point_normal_0 = packed_uint4(_S379.point_normal_0) ;

#line 359
        _S377->radiance_mw_0 = packed_uint4(_S379.radiance_mw_0) ;

        return;
    }

#line 361
    uint _S380 = _S368->frame_0;

    thread Rng_0 rng_9 = Rng_x24init_0(pcg_0(_S372 + pcg_0(_S371 + pcg_0(_S368->frame_0 * 8U + 6U))));

#line 363
    RadianceCache_0 _S381 = cache_of_0(_S367, int(0));

#line 363
    SkyFrame_natural_0 device* _S382 = _S368->sky_0;


    GiReservoir_0 _S383 = empty_gi_0();

#line 366
    thread GiReservoir_0 r_13 = _S383;
    if(gathers_this_frame_0(_S369, _S380, _S368->gi_rate_0))
    {

#line 367
        thread Frame_0 _S384 = _S376.frame_1;
        thread float pdf_4;

#line 368
        float3 _S385 = cosine_direction_0(&_S384, &rng_9, &pdf_4);

        float3 origin_3 = offset_origin_0(_S376.p_0, _S376.g_0, _S385);

#line 370
        Trace_0 _S386 = trace_closest_0(origin_3, _S385, 1.00000001504746622e+30f, &rng_9, &kernelContext_19);

        thread GiReservoir_0 sample_0 = _S383;
        (&sample_0)->M_0 = 1.0f;
        if(!_S386.hit_0)
        {

#line 375
            (&sample_0)->sky_1 = true;
            (&sample_0)->point_0 = _S385;

#line 376
            float3 _S387 = sky_radiance_0(_S382, int(0), _S385, false);
            (&sample_0)->radiance_2 = _S387;

#line 374
        }
        else
        {

#line 374
            TriangleHit_0 _S388 = surface_at_0(_S386.instance_1, _S386.primitive_2, _S386.bary_1, origin_3, &kernelContext_19);

#line 380
            (&sample_0)->point_0 = _S388.surface_4.position_0;
            set_normal_0(&sample_0, _S388.surface_4.geometric_normal_0);

#line 381
            thread RadianceCache_0 _S389 = _S381;
            thread uint level_offset_1;

#line 382
            bool _S390 = cache_lookup_0(&_S389, (&sample_0)->point_0, (&sample_0)->normal_4, &(&sample_0)->radiance_2, &level_offset_1, 0U);

#line 382
            bool _S391;

            if(_S390)
            {

#line 384
                _S391 = level_offset_1 == 0U;

#line 384
            }
            else
            {

#line 384
                _S391 = false;

#line 384
            }

#line 384
            thread RadianceCache_0 _S392 = _S381;

#line 384
            count_0(&_S392, 8U, _S391, &kernelContext_19);
            if(_S390)
            {

#line 385
                _S391 = level_offset_1 > 0U;

#line 385
            }
            else
            {

#line 385
                _S391 = false;

#line 385
            }

#line 385
            thread RadianceCache_0 _S393 = _S381;

#line 385
            count_0(&_S393, 9U, _S391, &kernelContext_19);
            bool _S394 = !_S390;

#line 386
            thread RadianceCache_0 _S395 = _S381;

#line 386
            count_0(&_S395, 10U, _S394, &kernelContext_19);
            if(_S394)
            {

#line 387
                thread Trace_0 _S396 = _S386;

#line 387
                DiffuseVertex_0 _S397 = diffuse_vertex_0(&_S396, origin_3, _S385, 0.0f, &kernelContext_19);

                if(_S397.lit_0)
                {

#line 389
                    thread DiffuseVertex_0 _S398 = _S397;

#line 389
                    float3 _S399 = diffuse_light_0(&_S398, &rng_9, &kernelContext_19);
                    (&sample_0)->radiance_2 = _S399;

#line 390
                    thread RadianceCache_0 _S400 = _S381;

#line 390
                    cache_add_0(&_S400, _S397.position_6, _S397.geometric_5, _S399, 0U, &kernelContext_19);

#line 389
                }

#line 387
            }

#line 374
        }

#line 374
        thread GiReservoir_0 _S401 = sample_0;

#line 374
        float _S402 = gi_source_pdf_0(&_S401, _S373.position_3, _S373.normal_3);

#line 374
        float _S403;

#line 397
        if(_S402 > 0.0f)
        {

#line 397
            thread GiReservoir_0 _S404 = sample_0;

#line 397
            float _S405 = gi_target_0(&_S404, _S373.position_3, _S373.normal_3);

#line 397
            _S403 = _S405 / _S402;

#line 397
        }
        else
        {

#line 397
            _S403 = 0.0f;

#line 397
        }

#line 397
        (&sample_0)->w_sum_0 = _S403;
        r_13 = sample_0;
        gi_finish_0(&r_13, _S373.position_3, _S373.normal_3);

#line 367
    }

#line 367
    PackedGiReservoir_natural_0 device* _S406 = _S368->gi_temporal_out_0+index_1;

#line 367
    thread GiReservoir_0 _S407 = r_13;

#line 367
    PackedGiReservoir_0 _S408 = pack_gi_0(&_S407);

#line 367
    _S406->point_normal_0 = packed_uint4(_S408.point_normal_0) ;

#line 367
    _S406->radiance_mw_0 = packed_uint4(_S408.radiance_mw_0) ;

#line 402
    return;
}

