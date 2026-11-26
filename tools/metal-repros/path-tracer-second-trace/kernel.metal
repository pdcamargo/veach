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
            

template<typename T>
void _slang_textureFence(T textureAsNonConst)
{
    textureAsNonConst.fence();
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


#line 14918 "hlsl.meta.slang"
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
    array<packed_float4, int(3)> data_0;
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


#line 37 "engine/render/shaders/veach/atmosphere.slang"
struct AtmosphereParams_natural_0
{
    packed_float4 rayleigh_0;
    packed_float4 mie_0;
    packed_float4 ozone_0;
    packed_float4 planet_0;
};


#line 37
struct _MatrixStorage_float4x4_ColMajornatural_0
{
    array<packed_float4, int(4)> data_1;
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


#line 4 "engine/render/shaders/veach/random.slang"
uint pcg_0(uint v_0)
{

#line 5
    uint state_0 = v_0 * 747796405U + 2891336453U;
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
    thread Rng_0 _S34;
    (&_S34)->state_1 = state_2;

#line 10
    return _S34;
}


#line 12
float Rng_next_0(Rng_0 thread* this_0)
{

#line 13
    uint _S35 = pcg_0(this_0->state_1);

#line 13
    this_0->state_1 = _S35;
    return float(_S35 >> 8U) * 5.9604644775390625e-08f;
}


#line 42 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/pt/path_tracer_variant.slang"
struct _MatrixStorage_float4x4_ColMajornatural_1
{
    array<float4, int(4)> data_2;
};


#line 29697 "hlsl.meta.slang"
struct Params_natural_0
{
    _MatrixStorage_float4x4_ColMajornatural_1 inverse_view_projection_1;
    float4 camera_position_exposure_0;
    float4 sky_bounces_0;
    texture2d<float, access::read_write> accumulation_0;
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> scene_0;
    InstanceRecord_natural_0 device* instances_0;
    MaterialRecord_natural_0 device* materials_0;
    LightRecord_natural_0 device* lights_0;
    EmissiveTriangle_natural_0 device* emissive_0;
    uint2 size_1;
    uint sample_index_0;
    uint samples_per_frame_0;
    uint light_count_0;
    uint emissive_count_0;
    float emissive_total_weight_0;
    uint mode_1;
    uint device* diagnostics_0;
    uint jitter_0;
    uint scene_flags_0;
    SkyFrame_natural_0 device* sky_0;
};


#line 61 "engine/render/shaders/veach/scene.slang"
struct SceneData_0
{
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> tlas_0;
    InstanceRecord_natural_0 device* instances_1;
    MaterialRecord_natural_0 device* materials_1;
    LightRecord_natural_0 device* lights_1;
    EmissiveTriangle_natural_0 device* emissive_1;
    uint light_count_1;
    uint emissive_count_1;
    float emissive_total_weight_1;
    uint debug_skip_0;
    uint flags_2;
};


#line 61
struct KernelContext_0
{
    Params_natural_0 constant* params_0;
    SceneData_0 veach_scene_0;
};


#line 188 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/pt/path_tracer_variant.slang"
float3 unproject_0(float2 ndc_0, float depth_0, KernelContext_0 thread* kernelContext_0)
{

#line 189
    float4 p_0 = (((float4(ndc_0, depth_0, 1.0f)) * (matrix<float,int(4),int(4)> (kernelContext_0->params_0->inverse_view_projection_1.data_2[int(0)][int(0)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(1)][int(0)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(2)][int(0)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(3)][int(0)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(0)][int(1)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(1)][int(1)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(2)][int(1)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(3)][int(1)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(0)][int(2)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(1)][int(2)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(2)][int(2)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(3)][int(2)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(0)][int(3)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(1)][int(3)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(2)][int(3)], kernelContext_0->params_0->inverse_view_projection_1.data_2[int(3)][int(3)]))));
    return p_0.xyz / float3(p_0.w) ;
}


#line 17 "engine/render/shaders/veach/material.slang"
SurfaceInput_0 SurfaceInput_x24init_0(float3 position_2, float3 geometric_normal_1, float3 normal_2, float4 tangent_1, float2 uv_2, float2 uv_dx_1, float2 uv_dy_1, float3 view_1)
{

#line 17
    thread SurfaceInput_0 _S36;
    (&_S36)->position_0 = position_2;
    (&_S36)->geometric_normal_0 = geometric_normal_1;
    (&_S36)->normal_0 = normal_2;
    (&_S36)->tangent_0 = tangent_1;
    (&_S36)->uv_0 = uv_2;
    (&_S36)->uv_dx_0 = uv_dx_1;
    (&_S36)->uv_dy_0 = uv_dy_1;
    (&_S36)->view_0 = view_1;

#line 17
    return _S36;
}


#line 57
MaterialContext_0 MaterialContext_x24init_0(const SurfaceInput_0 thread* surface_1, const MaterialRecord_natural_0 thread* record_1)
{

#line 57
    float4 _S37 = float4(record_1->base_color_0) ;

#line 57
    float4 _S38 = float4(record_1->emission_metallic_0) ;

#line 57
    float4 _S39 = float4(record_1->custom0_0) ;

#line 57
    float4 _S40 = float4(record_1->custom1_0) ;

#line 57
    thread MaterialContext_0 _S41;
    (&_S41)->surface_0 = *surface_1;

#line 58
    (&(&_S41)->record_0)->kind_0 = record_1->kind_0;

#line 58
    (&(&_S41)->record_0)->flags_0 = record_1->flags_0;

#line 58
    (&(&_S41)->record_0)->alpha_cutoff_0 = record_1->alpha_cutoff_0;

#line 58
    (&(&_S41)->record_0)->normal_scale_0 = record_1->normal_scale_0;

#line 58
    (&(&_S41)->record_0)->base_color_0 = _S37;

#line 58
    (&(&_S41)->record_0)->emission_metallic_0 = _S38;

#line 58
    (&(&_S41)->record_0)->roughness_0 = record_1->roughness_0;

#line 58
    (&(&_S41)->record_0)->emissive_weight_0 = record_1->emissive_weight_0;

#line 58
    (&(&_S41)->record_0)->sampler_0 = record_1->sampler_0;

#line 58
    (&(&_S41)->record_0)->base_color_texture_0 = record_1->base_color_texture_0;

#line 58
    (&(&_S41)->record_0)->metallic_roughness_texture_0 = record_1->metallic_roughness_texture_0;

#line 58
    (&(&_S41)->record_0)->normal_texture_0 = record_1->normal_texture_0;

#line 58
    (&(&_S41)->record_0)->emissive_texture_0 = record_1->emissive_texture_0;

#line 58
    (&(&_S41)->record_0)->custom0_0 = _S39;

#line 58
    (&(&_S41)->record_0)->custom1_0 = _S40;

#line 57
    return _S41;
}


#line 57
MaterialContext_0 MaterialContext_x24init_1(const SurfaceInput_0 thread* surface_2, const MaterialRecord_0 thread* record_2)
{

#line 57
    thread MaterialContext_0 _S42;
    (&_S42)->surface_0 = *surface_2;
    (&_S42)->record_0 = *record_2;

#line 57
    return _S42;
}


#line 13 "build/release/apps/lab/generated/veach/materials_generated.slang"
float evaluateAlpha_0(uint kind_2, const MaterialContext_0 thread* c_14)
{

#line 14
    switch(kind_2)
    {
    case 1U:
        {

#line 14
            float4 _S43 = IMaterial_evalBaseColorx24defaultImpl_1(c_14);
            return _S43.w;
        }
    default:
        {

#line 15
            float4 _S44 = IMaterial_evalBaseColorx24defaultImpl_0(c_14);
            return _S44.w;
        }
    }

#line 16
}


#line 16
uint slot_of_0(InstanceRecord_natural_0 device* _S45, uint _S46, uint _S47)
{

#line 106 "engine/render/shaders/veach/scene.slang"
    return (_S45+_S46)->slots_0[_S47];
}


#line 106
uint vertex_index_0(InstanceRecord_natural_0 device* _S48, uint _S49, uint _S50)
{

#line 103
    return (_S48+_S49)->indices_0[_S50];
}


#line 103
float vertex_float_0(InstanceRecord_natural_0 device* _S51, uint _S52, uint _S53)
{

#line 100
    return (_S51+_S52)->vertices_0[_S53];
}


#line 200
bool covered_0(uint instance_index_0, uint primitive_1, float2 bary_0, Rng_0 thread* rng_0, KernelContext_0 thread* kernelContext_1)
{

#line 201
    InstanceRecord_natural_0 device* _S54 = (&kernelContext_1->veach_scene_0)->instances_1;
    MaterialRecord_natural_0 device* _S55 = (&kernelContext_1->veach_scene_0)->materials_1;

#line 202
    uint _S56 = ((&kernelContext_1->veach_scene_0)->instances_1+instance_index_0)->material_base_0;

#line 202
    uint _S57 = slot_of_0((&kernelContext_1->veach_scene_0)->instances_1, instance_index_0, primitive_1);

#line 202
    thread MaterialRecord_natural_0 _S58 = _S55[_S56 + _S57];

#line 202
    uint _S59 = (&_S58)->flags_0;
    if((((&_S58)->flags_0) & 48U) == 0U)
    {

#line 204
        return true;
    }
    float _S60 = bary_0.x;

#line 206
    float _S61 = bary_0.y;

#line 206
    float3 _S62 = float3(1.0f - _S60 - _S61, _S60, _S61);
    float2 _S63 = float2(0.0f, 0.0f);

#line 207
    uint k_0 = 0U;

#line 207
    float2 uv_3 = _S63;
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
        uint _S64 = vertex_index_0(_S54, instance_index_0, primitive_1 * 3U + k_0);
        uint b_1 = _S64 * 12U + 10U;

#line 209
        float _S65 = vertex_float_0(_S54, instance_index_0, b_1);

#line 209
        float _S66 = vertex_float_0(_S54, instance_index_0, b_1 + 1U);
        float2 uv_4 = uv_3 + float2(_S65, _S66) * float2(_S62[k_0]) ;

#line 208
        k_0 = k_0 + 1U;

#line 208
        uv_3 = uv_4;

#line 208
    }

#line 18 "engine/render/shaders/veach/material.slang"
    float3 _S67 = float3(0.0f) ;



    float2 _S68 = float2(0.0f) ;

#line 212 "engine/render/shaders/veach/scene.slang"
    thread SurfaceInput_0 surface_3 = SurfaceInput_x24init_0(_S67, _S67, _S67, float4(0.0f) , _S68, _S68, _S68, _S67);
    (&surface_3)->uv_0 = uv_3;

#line 213
    thread SurfaceInput_0 _S69 = surface_3;

#line 213
    MaterialContext_0 _S70 = MaterialContext_x24init_0(&_S69, &_S58);

#line 213
    thread MaterialContext_0 _S71 = _S70;

#line 213
    float _S72 = evaluateAlpha_0((&_S58)->kind_0, &_S71);


    if((_S59 & 16U) != 0U)
    {

#line 217
        return _S72 >= ((&_S58)->alpha_cutoff_0);
    }
    float _S73 = Rng_next_0(rng_0);

#line 219
    return _S73 < _S72;
}

struct Trace_0
{
    bool hit_0;
    uint instance_1;
    uint primitive_2;
    float2 bary_1;
    float t_2;
};


#line 236
Trace_0 trace_0(float3 origin_0, float3 direction_1, float tmax_0, Rng_0 thread* rng_1, uint instance_mask_0, KernelContext_0 thread* kernelContext_2)
{

#line 237
    thread Trace_0 result_0;
    (&result_0)->hit_0 = false;
    (&result_0)->instance_1 = 0U;
    (&result_0)->primitive_2 = 0U;
    (&result_0)->bary_1 = float2(0.0f, 0.0f);
    (&result_0)->t_2 = 0.0f;



    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> query_0;

#line 246
    float tmin_0 = 0.0f;

#line 246
    uint layer_0 = 0U;


    for(;;)
    {

#line 249
        if(layer_0 < 64U)
        {
        }
        else
        {

#line 249
            break;
        }

#line 250
        thread RayDesc ray_0;
        (&ray_0)->Origin = origin_0;
        (&ray_0)->Direction = direction_1;
        (&ray_0)->TMin = tmin_0;
        (&ray_0)->TMax = tmax_0;
        metal::raytracing::acceleration_structure<metal::raytracing::instancing> _S74 = getDescriptorFromHandle_0((&kernelContext_2->veach_scene_0)->tlas_0);

#line 255
        (&query_0)->reset({ (ray_0.Origin), (ray_0.Direction), (ray_0.TMin), (ray_0.TMax) }, (_S74), (instance_mask_0), _slang_ray_flags_to_intersection_params((1U)));
        for(;;)
        {

#line 256
            bool _S75 = ((&query_0)->next());

#line 256
            if(_S75)
            {
            }
            else
            {

#line 256
                break;
            }

#line 256
        }

        uint _S76 = (_slang_committed_intersection_type_to_uint((&query_0)->get_committed_intersection_type()));

#line 258
        if(_S76 != 1U)
        {

#line 259
            (&result_0)->hit_0 = false;
            return result_0;
        }
        uint instance_2 = ((&query_0)->get_committed_instance_id());
        uint primitive_3 = ((&query_0)->get_committed_primitive_id());
        float2 bary_2 = ((&query_0)->get_committed_triangle_barycentric_coord());
        float t_3 = ((&query_0)->get_committed_distance());

#line 265
        bool opaque_0;


        if(((((&kernelContext_2->veach_scene_0)->instances_1+instance_2)->flags_1) & 1U) == 0U)
        {

#line 268
            opaque_0 = true;

#line 268
        }
        else
        {

#line 268
            opaque_0 = (((&kernelContext_2->veach_scene_0)->debug_skip_0) & 2U) != 0U;

#line 268
        }

        (&result_0)->hit_0 = true;
        (&result_0)->instance_1 = instance_2;
        (&result_0)->primitive_2 = primitive_3;
        (&result_0)->bary_1 = bary_2;
        (&result_0)->t_2 = t_3;

#line 274
        bool _S77;
        if(opaque_0)
        {

#line 275
            _S77 = true;

#line 275
        }
        else
        {

#line 275
            bool _S78 = covered_0(instance_2, primitive_3, bary_2, rng_1, kernelContext_2);

#line 275
            _S77 = _S78;

#line 275
        }

#line 275
        if(_S77)
        {

#line 276
            return result_0;
        }
        float _S79 = t_3 * 1.0000100135803223f + 9.9999997473787516e-06f;

#line 249
        uint layer_1 = layer_0 + 1U;

#line 249
        tmin_0 = _S79;

#line 249
        layer_0 = layer_1;

#line 249
    }

#line 280
    return result_0;
}


#line 73 "engine/render/shaders/veach/atmosphere.slang"
float ray_sphere_0(float3 origin_1, float3 direction_2, float radius_0)
{

#line 74
    float b_2 = dot(origin_1, direction_2);

    float discriminant_0 = b_2 * b_2 - (dot(origin_1, origin_1) - radius_0 * radius_0);
    if(discriminant_0 < 0.0f)
    {

#line 78
        return -1.0f;
    }
    float s_3 = sqrt(discriminant_0);
    float _S80 = - b_2;

#line 81
    float t0_0 = _S80 - s_3;
    float t1_0 = _S80 + s_3;

#line 82
    float _S81;
    if(t0_0 >= 0.0f)
    {

#line 83
        _S81 = t0_0;

#line 83
    }
    else
    {

#line 83
        if(t1_0 >= 0.0f)
        {

#line 83
            _S81 = t1_0;

#line 83
        }
        else
        {

#line 83
            _S81 = -1.0f;

#line 83
        }

#line 83
    }

#line 83
    return _S81;
}



float2 transmittance_uv_0(const AtmosphereParams_natural_0 thread* a_0, float r_0, float mu_0)
{

#line 88
    float4 _S82 = float4(a_0->planet_0) ;
    float bottom_0 = _S82.x;
    float top_0 = _S82.y;
    float _S83 = top_0 * top_0;

#line 91
    float _S84 = bottom_0 * bottom_0;

#line 91
    float h_0 = sqrt(max(_S83 - _S84, 0.0f));
    float _S85 = r_0 * r_0;

#line 92
    float rho_0 = sqrt(max(_S85 - _S84, 0.0f));


    float d_min_0 = top_0 - r_0;

    return float2((max(0.0f, - r_0 * mu_0 + sqrt(max(_S85 * (mu_0 * mu_0 - 1.0f) + _S83, 0.0f))) - d_min_0) / max(rho_0 + h_0 - d_min_0, 9.99999997475242708e-07f), rho_0 / h_0);
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
TriangleRef_0 triangle_of_0(InstanceRecord_natural_0 device* _S86, uint _S87, uint _S88)
{

#line 134
    thread TriangleRef_0 t_4;
    uint _S89 = _S88 * 3U;

#line 135
    uint _S90 = vertex_index_0(_S86, _S87, _S89);

#line 135
    uint _S91 = vertex_index_0(_S86, _S87, _S89 + 1U);

#line 135
    uint _S92 = vertex_index_0(_S86, _S87, _S89 + 2U);

#line 135
    (&t_4)->vertices_1 = uint3(_S90, _S91, _S92);

#line 135
    uint _S93 = slot_of_0(_S86, _S87, _S88);

    (&t_4)->slot_0 = _S93;
    return t_4;
}


#line 91
struct Corner_0
{
    float3 position_3;
    float3 normal_3;
    float4 tangent_2;
    float2 uv_5;
};


#line 91
Corner_0 fetch_0(InstanceRecord_natural_0 device* _S94, uint _S95, uint _S96)
{

#line 110
    uint b_3 = _S96 * 12U;
    thread Corner_0 c_15;

#line 111
    float _S97 = vertex_float_0(_S94, _S95, b_3);

#line 111
    float _S98 = vertex_float_0(_S94, _S95, b_3 + 1U);

#line 111
    float _S99 = vertex_float_0(_S94, _S95, b_3 + 2U);
    (&c_15)->position_3 = float3(_S97, _S98, _S99);

#line 112
    float _S100 = vertex_float_0(_S94, _S95, b_3 + 3U);

#line 112
    float _S101 = vertex_float_0(_S94, _S95, b_3 + 4U);

#line 112
    float _S102 = vertex_float_0(_S94, _S95, b_3 + 5U);
    (&c_15)->normal_3 = float3(_S100, _S101, _S102);

#line 113
    float _S103 = vertex_float_0(_S94, _S95, b_3 + 6U);

#line 113
    float _S104 = vertex_float_0(_S94, _S95, b_3 + 7U);

#line 113
    float _S105 = vertex_float_0(_S94, _S95, b_3 + 8U);

#line 113
    float _S106 = vertex_float_0(_S94, _S95, b_3 + 9U);
    (&c_15)->tangent_2 = float4(_S103, _S104, _S105, _S106);

#line 114
    float _S107 = vertex_float_0(_S94, _S95, b_3 + 10U);

#line 114
    float _S108 = vertex_float_0(_S94, _S95, b_3 + 11U);
    (&c_15)->uv_5 = float2(_S107, _S108);
    return c_15;
}


#line 116
float3 to_world_0(InstanceRecord_natural_0 device* _S109, uint _S110, float3 _S111, float _S112)
{

#line 87
    float4 v_1 = float4(_S111, _S112);
    return float3(dot(float4((&(_S109+_S110)->object_to_world_0)->data_0[int(0)]) , v_1), dot(float4((&(_S109+_S110)->object_to_world_0)->data_0[int(1)]) , v_1), dot(float4((&(_S109+_S110)->object_to_world_0)->data_0[int(2)]) , v_1));
}


#line 88
float3 to_world_1(InstanceRecord_natural_0 device* _S113, uint _S114, float3 _S115, float _S116)
{

#line 87
    float4 v_2 = float4(_S115, _S116);
    return float3(dot(float4((&(_S113+_S114)->normal_to_world_0)->data_0[int(0)]) , v_2), dot(float4((&(_S113+_S114)->normal_to_world_0)->data_0[int(1)]) , v_2), dot(float4((&(_S113+_S114)->normal_to_world_0)->data_0[int(2)]) , v_2));
}


#line 88
TriangleHit_0 surface_of_0(InstanceRecord_natural_0 device* _S117, uint _S118, const TriangleRef_0 thread* _S119, float2 _S120, float3 _S121, KernelContext_0 thread* kernelContext_3)
{

#line 88
    InstanceRecord_natural_0 device* _S122 = _S117+_S118;

#line 88
    uint3 _S123 = _S119->vertices_1;

#line 88
    Corner_0 _S124 = fetch_0(_S117, _S118, _S119->vertices_1.x);

#line 88
    Corner_0 _S125 = fetch_0(_S117, _S118, _S123.y);

#line 88
    Corner_0 _S126 = fetch_0(_S117, _S118, _S123.z);

#line 170
    float _S127 = _S120.x;

#line 170
    float _S128 = _S120.y;

#line 170
    float _S129 = 1.0f - _S127 - _S128;

#line 170
    float3 _S130 = to_world_0(_S117, _S118, _S124.position_3, 1.0f);

#line 170
    float3 _S131 = to_world_0(_S117, _S118, _S125.position_3, 1.0f);

#line 170
    float3 _S132 = to_world_0(_S117, _S118, _S126.position_3, 1.0f);

#line 175
    float3 cross_edges_0 = cross(_S131 - _S130, _S132 - _S130);

#line 174
    thread TriangleHit_0 hit_1;

    (&hit_1)->area_0 = 0.5f * length(cross_edges_0);
    float3 geometric_0 = normalize(cross_edges_0);

#line 177
    float3 _S133 = float3(_S129) ;

#line 177
    float3 _S134 = float3(_S127) ;

#line 177
    float3 _S135 = float3(_S128) ;
    float3 _S136 = _S130 * _S133 + _S131 * _S134 + _S132 * _S135;

#line 178
    (&(&hit_1)->surface_4)->position_0 = _S136;
    float3 _S137 = normalize(_S121 - _S136);

#line 179
    (&(&hit_1)->surface_4)->view_0 = _S137;

#line 179
    float3 _S138 = to_world_1(_S117, _S118, _S124.normal_3 * _S133 + _S125.normal_3 * _S134 + _S126.normal_3 * _S135, 0.0f);
    float3 normal_4 = normalize(_S138);
    float4 t_5 = _S124.tangent_2 * float4(_S129)  + _S125.tangent_2 * float4(_S127)  + _S126.tangent_2 * float4(_S128) ;

#line 181
    float3 _S139 = to_world_0(_S117, _S118, t_5.xyz, 0.0f);
    (&(&hit_1)->surface_4)->tangent_0 = float4(_S139, t_5.w);
    (&(&hit_1)->surface_4)->uv_0 = _S124.uv_5 * float2(_S129)  + _S125.uv_5 * float2(_S127)  + _S126.uv_5 * float2(_S128) ;
    float2 _S140 = float2(0.0f, 0.0f);

#line 184
    (&(&hit_1)->surface_4)->uv_dx_0 = _S140;
    (&(&hit_1)->surface_4)->uv_dy_0 = _S140;

    bool _S141 = (dot(geometric_0, _S137)) >= 0.0f;

#line 187
    (&hit_1)->front_0 = _S141;

#line 187
    float3 geometric_1;

#line 187
    float3 normal_5;
    if(!_S141)
    {
        float3 _S142 = - normal_4;

#line 190
        geometric_1 = - geometric_0;

#line 190
        normal_5 = _S142;

#line 188
    }
    else
    {

#line 188
        geometric_1 = geometric_0;

#line 188
        normal_5 = normal_4;

#line 188
    }



    (&(&hit_1)->surface_4)->geometric_normal_0 = geometric_1;
    (&(&hit_1)->surface_4)->normal_0 = normal_5;
    MaterialRecord_natural_0 _S143 = (&kernelContext_3->veach_scene_0)->materials_1[_S122->material_base_0 + _S119->slot_0];

#line 194
    float4 _S144 = float4(_S143.base_color_0) ;

#line 194
    float4 _S145 = float4(_S143.emission_metallic_0) ;

#line 194
    float4 _S146 = float4(_S143.custom0_0) ;

#line 194
    float4 _S147 = float4(_S143.custom1_0) ;

#line 194
    (&(&hit_1)->material_0)->kind_0 = _S143.kind_0;

#line 194
    (&(&hit_1)->material_0)->flags_0 = _S143.flags_0;

#line 194
    (&(&hit_1)->material_0)->alpha_cutoff_0 = _S143.alpha_cutoff_0;

#line 194
    (&(&hit_1)->material_0)->normal_scale_0 = _S143.normal_scale_0;

#line 194
    (&(&hit_1)->material_0)->base_color_0 = _S144;

#line 194
    (&(&hit_1)->material_0)->emission_metallic_0 = _S145;

#line 194
    (&(&hit_1)->material_0)->roughness_0 = _S143.roughness_0;

#line 194
    (&(&hit_1)->material_0)->emissive_weight_0 = _S143.emissive_weight_0;

#line 194
    (&(&hit_1)->material_0)->sampler_0 = _S143.sampler_0;

#line 194
    (&(&hit_1)->material_0)->base_color_texture_0 = _S143.base_color_texture_0;

#line 194
    (&(&hit_1)->material_0)->metallic_roughness_texture_0 = _S143.metallic_roughness_texture_0;

#line 194
    (&(&hit_1)->material_0)->normal_texture_0 = _S143.normal_texture_0;

#line 194
    (&(&hit_1)->material_0)->emissive_texture_0 = _S143.emissive_texture_0;

#line 194
    (&(&hit_1)->material_0)->custom0_0 = _S146;

#line 194
    (&(&hit_1)->material_0)->custom1_0 = _S147;
    return hit_1;
}


#line 159
TriangleHit_0 surface_at_0(uint instance_index_1, uint primitive_4, float2 bary_3, float3 view_origin_0, KernelContext_0 thread* kernelContext_4)
{

#line 160
    InstanceRecord_natural_0 device* _S148 = (&kernelContext_4->veach_scene_0)->instances_1;

#line 160
    TriangleRef_0 _S149 = triangle_of_0((&kernelContext_4->veach_scene_0)->instances_1, instance_index_1, primitive_4);

#line 160
    thread TriangleRef_0 _S150 = _S149;

#line 160
    TriangleHit_0 _S151 = surface_of_0(_S148, instance_index_1, &_S150, bary_3, view_origin_0, kernelContext_4);
    return _S151;
}


#line 5 "build/release/apps/lab/generated/veach/materials_generated.slang"
SurfaceSample_0 evaluateSurface_0(uint kind_3, const MaterialContext_0 thread* c_16)
{

#line 6
    switch(kind_3)
    {
    case 1U:
        {

#line 6
            SurfaceSample_0 _S152 = evaluate_1(c_16);
            return _S152;
        }
    default:
        {

#line 7
            SurfaceSample_0 _S153 = evaluate_0(c_16);
            return _S153;
        }
    }

#line 8
}


#line 10 "engine/render/shaders/veach/lights.slang"
float emissive_selection_probability_0(KernelContext_0 thread* kernelContext_5)
{

#line 10
    float _S154;
    if(((&kernelContext_5->veach_scene_0)->emissive_count_1) == 0U)
    {

#line 11
        _S154 = 0.0f;

#line 11
    }
    else
    {

#line 11
        if(((&kernelContext_5->veach_scene_0)->light_count_1) == 0U)
        {

#line 11
            _S154 = 1.0f;

#line 11
        }
        else
        {

#line 11
            _S154 = 0.5f;

#line 11
        }

#line 11
    }

#line 11
    return _S154;
}


float emissive_light_pdf_0(const MaterialRecord_0 thread* m_1, float area_1, float distance_0, float cos_light_0, KernelContext_0 thread* kernelContext_6)
{

#line 15
    bool _S155;
    if(((&kernelContext_6->veach_scene_0)->emissive_total_weight_1) <= 0.0f)
    {

#line 16
        _S155 = true;

#line 16
    }
    else
    {

#line 16
        _S155 = cos_light_0 <= 0.0f;

#line 16
    }

#line 16
    if(_S155)
    {

#line 17
        return 0.0f;
    }

#line 17
    float _S156 = emissive_selection_probability_0(kernelContext_6);


    return _S156 * m_1->emissive_weight_0 / (&kernelContext_6->veach_scene_0)->emissive_total_weight_1 * distance_0 * distance_0 / cos_light_0;
}


#line 139 "engine/render/shaders/veach/brdf.slang"
float power_heuristic_0(float a_1, float b_4)
{

#line 140
    float _S157 = a_1 * a_1;

#line 140
    return _S157 / max(_S157 + b_4 * b_4, 9.99999968265522539e-21f);
}


#line 13
struct Frame_0
{
    float3 t_6;
    float3 b_5;
    float3 n_1;
};


Frame_0 frame_of_0(float3 n_2)
{

#line 22
    float _S158 = n_2.z;

#line 22
    float s_4;

#line 22
    if(_S158 >= 0.0f)
    {

#line 22
        s_4 = 1.0f;

#line 22
    }
    else
    {

#line 22
        s_4 = -1.0f;

#line 22
    }
    float a_2 = -1.0f / (s_4 + _S158);
    float _S159 = n_2.x;

#line 24
    float _S160 = n_2.y;

#line 24
    float b_6 = _S159 * _S160 * a_2;
    thread Frame_0 f_0;
    (&f_0)->t_6 = float3(1.0f + s_4 * _S159 * _S159 * a_2, s_4 * b_6, - s_4 * _S159);
    (&f_0)->b_5 = float3(b_6, s_4 + _S160 * _S160 * a_2, - _S160);
    (&f_0)->n_1 = n_2;
    return f_0;
}


#line 18
float3 Frame_to_world_0(const Frame_0 thread* this_1, float3 v_3)
{

#line 18
    return float3(v_3.x)  * this_1->t_6 + float3(v_3.y)  * this_1->b_5 + float3(v_3.z)  * this_1->n_1;
}


#line 320 "engine/render/shaders/veach/scene.slang"
float3 offset_origin_0(float3 p_1, float3 geometric_2, float3 direction_3)
{

#line 320
    float _S161;
    if((dot(geometric_2, direction_3)) >= 0.0f)
    {

#line 321
        _S161 = 0.0010000000474974513f;

#line 321
    }
    else
    {

#line 321
        _S161 = -0.0010000000474974513f;

#line 321
    }

#line 321
    return p_1 + geometric_2 * float3(_S161) ;
}


#line 328
bool visible_0(float3 from_0, float3 to_direction_0, float distance_1, Rng_0 thread* rng_2, KernelContext_0 thread* kernelContext_7)
{

#line 329
    bool skip_alpha_0 = (((&kernelContext_7->veach_scene_0)->debug_skip_0) & 2U) != 0U;
    thread RayDesc ray_1;
    (&ray_1)->Origin = from_0;
    (&ray_1)->Direction = to_direction_0;
    (&ray_1)->TMin = 0.0f;
    (&ray_1)->TMax = distance_1;
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> opaque_1;
    metal::raytracing::acceleration_structure<metal::raytracing::instancing> _S162 = getDescriptorFromHandle_0((&kernelContext_7->veach_scene_0)->tlas_0);

#line 336
    uint _S163;
    if(skip_alpha_0)
    {

#line 337
        _S163 = 255U;

#line 337
    }
    else
    {

#line 337
        _S163 = 1U;

#line 337
    }

#line 336
    (&opaque_1)->reset({ (ray_1.Origin), (ray_1.Direction), (ray_1.TMin), (ray_1.TMax) }, (_S162), (_S163), _slang_ray_flags_to_intersection_params((5U)));

    for(;;)
    {

#line 338
        bool _S164 = ((&opaque_1)->next());

#line 338
        if(_S164)
        {
        }
        else
        {

#line 338
            break;
        }

#line 338
    }

    uint _S165 = (_slang_committed_intersection_type_to_uint((&opaque_1)->get_committed_intersection_type()));

#line 340
    if(_S165 == 1U)
    {

#line 341
        return false;
    }

#line 341
    bool _S166;

    if(skip_alpha_0)
    {

#line 343
        _S166 = true;

#line 343
    }
    else
    {

#line 343
        _S166 = (((&kernelContext_7->veach_scene_0)->flags_2) & 1U) == 0U;

#line 343
    }

#line 343
    if(_S166)
    {

#line 344
        return true;
    }

    metal::raytracing::acceleration_structure<metal::raytracing::instancing> _S167 = getDescriptorFromHandle_0((&kernelContext_7->veach_scene_0)->tlas_0);

#line 346
    thread raytracing::intersection_query<raytracing::triangle_data, raytracing::instancing> alpha_1;
    (&alpha_1)->reset({ (ray_1.Origin), (ray_1.Direction), (ray_1.TMin), (ray_1.TMax) }, (_S167), (2U), _slang_ray_flags_to_intersection_params((4U)));
    for(;;)
    {

#line 348
        bool _S168 = ((&alpha_1)->next());

#line 348
        if(_S168)
        {
        }
        else
        {

#line 348
            break;
        }

#line 349
        uint _S169 = (
                        _slang_candidate_intersection_type_to_uint((&alpha_1)->get_candidate_intersection_type())
                    );

#line 349
        if(_S169 == 0U)
        {

#line 350
            uint _S170 = ((&alpha_1)->get_candidate_instance_id());

#line 350
            uint _S171 = ((&alpha_1)->get_candidate_primitive_id());
            float2 _S172 = ((&alpha_1)->get_candidate_triangle_barycentric_coord());

#line 351
            bool _S173 = covered_0(_S170, _S171, _S172, rng_2, kernelContext_7);

#line 351
            _S166 = _S173;

#line 349
        }
        else
        {

#line 349
            _S166 = false;

#line 349
        }

#line 349
        if(_S166)
        {

            (&alpha_1)->commit_triangle_intersection();

#line 349
        }

#line 348
    }

#line 355
    uint _S174 = (_slang_committed_intersection_type_to_uint((&alpha_1)->get_committed_intersection_type()));

#line 355
    return _S174 != 1U;
}


#line 17 "engine/render/shaders/veach/brdf.slang"
float3 Frame_to_local_0(const Frame_0 thread* this_2, float3 v_4)
{

#line 17
    return float3(dot(v_4, this_2->t_6), dot(v_4, this_2->b_5), dot(v_4, this_2->n_1));
}


#line 9
float luminance_0(float3 c_17)
{

#line 10
    return dot(c_17, float3(0.2125999927520752f, 0.71520000696182251f, 0.072200000286102295f));
}


#line 32
struct Brdf_0
{
    float3 diffuse_0;
    float3 f0_0;
    float alpha_2;
    float specular_probability_0;
};


#line 39
Brdf_0 brdf_of_0(const SurfaceSample_0 thread* s_5, float n_dot_v_0)
{

#line 40
    thread Brdf_0 b_7;
    float3 _S175 = s_5->base_color_1 * float3((1.0f - s_5->metallic_0)) ;

#line 41
    (&b_7)->diffuse_0 = _S175;
    float3 _S176 = mix(float3(0.039999999105930328f, 0.039999999105930328f, 0.039999999105930328f), s_5->base_color_1, float3(s_5->metallic_0) );

#line 42
    (&b_7)->f0_0 = _S176;
    float _S177 = max(s_5->roughness_1, 0.019999999552965164f);
    (&b_7)->alpha_2 = _S177 * _S177;
    float spec_0 = luminance_0(_S176 + (float3(1.0f)  - _S176) * float3(pow(1.0f - n_dot_v_0, 5.0f)) );

    float _S178 = spec_0 + luminance_0(_S175);

#line 47
    float _S179;

#line 47
    if(_S178 > 0.0f)
    {

#line 47
        _S179 = clamp(spec_0 / _S178, 0.10000000149011612f, 0.89999997615814209f);

#line 47
    }
    else
    {

#line 47
        _S179 = 0.5f;

#line 47
    }

#line 47
    (&b_7)->specular_probability_0 = _S179;
    return b_7;
}


#line 136 "engine/render/shaders/veach/lights.slang"
uint pick_emissive_0(float u_0, KernelContext_0 thread* kernelContext_8)
{
    uint _S180 = (&kernelContext_8->veach_scene_0)->emissive_count_1 - 1U;

#line 138
    uint lo_0 = 0U;

#line 138
    uint hi_0 = _S180;
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
        if(((&kernelContext_8->veach_scene_0)->emissive_1[mid_0].cdf_0) < u_0)
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
    bool _S181;
    if(!hit_2->front_0)
    {

#line 24
        _S181 = (((&hit_2->material_0)->flags_0) & 64U) == 0U;

#line 24
    }
    else
    {

#line 24
        _S181 = false;

#line 24
    }

#line 24
    if(_S181)
    {

#line 25
        return float3(0.0f, 0.0f, 0.0f);
    }

#line 25
    MaterialContext_0 _S182 = MaterialContext_x24init_1(&hit_2->surface_4, &hit_2->material_0);

#line 25
    thread MaterialContext_0 _S183 = _S182;

#line 25
    SurfaceSample_0 _S184 = evaluateSurface_0((&hit_2->material_0)->kind_0, &_S183);


    return _S184.emission_0;
}


#line 81 "engine/render/shaders/veach/brdf.slang"
float3 lambert_eval_0(float3 albedo_0, float3 l_0, float thread* pdf_0)
{

#line 82
    float _S185 = max(l_0.z, 0.0f) / 3.1415927410125732f;

#line 82
    *pdf_0 = _S185;
    return albedo_0 * float3(_S185) ;
}


#line 51
float ggx_d_0(float n_dot_h_0, float a2_0)
{

#line 52
    float d_0 = n_dot_h_0 * n_dot_h_0 * (a2_0 - 1.0f) + 1.0f;
    return a2_0 / (3.1415927410125732f * d_0 * d_0);
}

float smith_g1_0(float n_dot_x_0, float a2_1)
{

#line 57
    return 2.0f * n_dot_x_0 / (n_dot_x_0 + sqrt(a2_1 + (1.0f - a2_1) * n_dot_x_0 * n_dot_x_0));
}


float3 brdf_eval_0(const Brdf_0 thread* b_8, float3 v_5, float3 l_1, float thread* pdf_1)
{

#line 62
    *pdf_1 = 0.0f;
    float _S186 = l_1.z;

#line 63
    bool _S187;

#line 63
    if(_S186 <= 0.0f)
    {

#line 63
        _S187 = true;

#line 63
    }
    else
    {

#line 63
        _S187 = (v_5.z) <= 0.0f;

#line 63
    }

#line 63
    if(_S187)
    {

#line 64
        return float3(0.0f, 0.0f, 0.0f);
    }
    float3 h_1 = normalize(v_5 + l_1);
    float a2_2 = b_8->alpha_2 * b_8->alpha_2;
    float d_1 = ggx_d_0(h_1.z, a2_2);
    float _S188 = v_5.z;

#line 69
    float _S189 = 1.0f - a2_2;

#line 69
    float3 _S190 = float3(1.0f) ;
    float3 f_1 = b_8->f0_0 + (_S190 - b_8->f0_0) * float3(pow(1.0f - saturate(dot(v_5, h_1)), 5.0f)) ;
    float3 specular_0 = float3((d_1 * (0.5f / (_S186 * sqrt(_S188 * _S188 * _S189 + a2_2) + _S188 * sqrt(_S186 * _S186 * _S189 + a2_2)))))  * f_1;
    float3 diffuse_1 = (_S190 - f_1) * b_8->diffuse_0 / float3(3.1415927410125732f) ;


    *pdf_1 = b_8->specular_probability_0 * (smith_g1_0(_S188, a2_2) * d_1 / (4.0f * _S188)) + (1.0f - b_8->specular_probability_0) * (_S186 / 3.1415927410125732f);
    return (specular_0 + diffuse_1) * float3(_S186) ;
}


#line 32 "engine/render/shaders/veach/lights.slang"
float3 punctual_irradiance_0(const LightRecord_natural_0 thread* light_0, float3 p_2, float3 thread* l_2, float thread* distance_2)
{

#line 33
    float3 irradiance_1 = (float4(light_0->radiance_0) ).xyz;

#line 33
    uint _S191 = light_0->kind_1;

#line 33
    float3 irradiance_2;
    if((light_0->kind_1) == 0U)
    {

#line 35
        *l_2 = - (float4(light_0->direction_0) ).xyz;
        *distance_2 = 1.00000001504746622e+30f;

#line 36
        irradiance_2 = irradiance_1;

#line 34
    }
    else
    {

        float3 to_light_0 = (float4(light_0->position_1) ).xyz - p_2;
        float _S192 = length(to_light_0);

#line 39
        *distance_2 = _S192;
        *l_2 = to_light_0 / float3(_S192) ;
        float3 irradiance_3 = irradiance_1 / float3((*distance_2 * *distance_2)) ;
        if(_S191 == 2U)
        {
            float falloff_0 = saturate((dot((float4(light_0->direction_0) ).xyz, - *l_2) - light_0->cos_outer_0) / max(light_0->cos_inner_0 - light_0->cos_outer_0, 9.9999997473787516e-05f));

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
void sample_light_extent_0(const LightRecord_natural_0 thread* light_1, float2 u_1, float3 thread* point_0, float3 thread* normal_6)
{

#line 59
    if((light_1->kind_1) == 0U)
    {

#line 60
        float cos_theta_0 = 1.0f - u_1.x * (1.0f - cos(light_1->size_0));
        float sin_theta_0 = sqrt(max(1.0f - cos_theta_0 * cos_theta_0, 0.0f));

        float _S193 = 6.2831854820251465f * u_1.y;

#line 63
        float3 _S194 = float3(sin_theta_0 * cos(_S193), sin_theta_0 * sin(_S193), cos_theta_0);

#line 63
        thread Frame_0 _S195 = frame_of_0(- (float4(light_1->direction_0) ).xyz);

#line 63
        float3 _S196 = Frame_to_world_0(&_S195, _S194);

#line 63
        *normal_6 = _S196;
        *point_0 = float3(0.0f, 0.0f, 0.0f);
        return;
    }
    float z_0 = 1.0f - 2.0f * u_1.x;
    float r_1 = sqrt(max(1.0f - z_0 * z_0, 0.0f));
    float _S197 = 6.2831854820251465f * u_1.y;

#line 69
    float3 _S198 = float3(r_1 * cos(_S197), r_1 * sin(_S197), z_0);

#line 69
    *normal_6 = _S198;
    *point_0 = (float4(light_1->position_1) ).xyz + float3(light_1->size_0)  * _S198;
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



float3 light_extent_incoming_0(const LightRecord_natural_0 thread* light_3, float3 point_1, float3 normal_7, float3 p_3, float3 thread* l_3, float thread* distance_3)
{

#line 83
    uint _S199 = light_3->kind_1;
    if((light_3->kind_1) == 0U)
    {

#line 85
        *l_3 = normal_7;
        *distance_3 = 1.00000001504746622e+30f;
        float3 _S200 = (float4(light_3->radiance_0) ).xyz;

#line 87
        float _S201 = light_extent_pdf_0(light_3);

#line 87
        return _S200 * float3(_S201) ;
    }
    float3 to_light_1 = point_1 - p_3;
    float _S202 = length(to_light_1);

#line 90
    *distance_3 = _S202;

#line 90
    float3 _S203;
    if(_S202 > 9.99999997475242708e-07f)
    {

#line 91
        _S203 = to_light_1 / float3(*distance_3) ;

#line 91
    }
    else
    {

#line 91
        _S203 = float3(0.0f, 0.0f, 1.0f);

#line 91
    }

#line 91
    *l_3 = _S203;
    float cos_light_1 = dot(normal_7, - _S203);

#line 92
    bool _S204;
    if((*distance_3) <= 9.99999997475242708e-07f)
    {

#line 93
        _S204 = true;

#line 93
    }
    else
    {

#line 93
        _S204 = cos_light_1 <= 0.0f;

#line 93
    }

#line 93
    if(_S204)
    {

#line 94
        return float3(0.0f, 0.0f, 0.0f);
    }

#line 94
    float falloff_1;


    if(_S199 == 2U)
    {
        float falloff_2 = saturate((dot((float4(light_3->direction_0) ).xyz, normalize(p_3 - (float4(light_3->position_1) ).xyz)) - light_3->cos_outer_0) / max(light_3->cos_inner_0 - light_3->cos_outer_0, 9.9999997473787516e-05f));

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
    return (float4(light_3->radiance_0) ).xyz * float3(falloff_1)  / float3((3.1415927410125732f * light_3->size_0 * light_3->size_0))  * float3(cos_light_1)  / float3((*distance_3 * *distance_3)) ;
}



float3 punctual_irradiance_sample_0(const LightRecord_natural_0 thread* light_4, float3 p_4, Rng_0 thread* rng_3, float3 thread* l_4, float thread* distance_4)
{

#line 109
    if((light_4->size_0) <= 0.0f)
    {

#line 109
        float3 _S205 = punctual_irradiance_0(light_4, p_4, l_4, distance_4);
        return _S205;
    }


    float _S206 = Rng_next_0(rng_3);

#line 114
    float _S207 = Rng_next_0(rng_3);

#line 112
    thread float3 point_2;
    thread float3 normal_8;

#line 113
    sample_light_extent_0(light_4, float2(_S206, _S207), &point_2, &normal_8);

#line 113
    float3 _S208 = light_extent_incoming_0(light_4, point_2, normal_8, p_4, l_4, distance_4);

#line 113
    float _S209 = light_extent_pdf_0(light_4);

    return _S208 / float3(_S209) ;
}


float3 sample_punctual_0(float3 p_5, const Frame_0 thread* frame_0, const Brdf_0 thread* b_9, float3 v_local_0, float p_punctual_0, Rng_0 thread* rng_4, float3 thread* l_5, float thread* distance_5, bool lambert_0, KernelContext_0 thread* kernelContext_9)
{
    *l_5 = float3(0.0f, 0.0f, 1.0f);
    *distance_5 = 0.0f;
    if(((&kernelContext_9->veach_scene_0)->light_count_1) == 0U)
    {

#line 124
        return float3(0.0f, 0.0f, 0.0f);
    }
    float _S210 = Rng_next_0(rng_4);
    float select_0 = p_punctual_0 / float((&kernelContext_9->veach_scene_0)->light_count_1);

#line 127
    thread LightRecord_natural_0 _S211 = (&kernelContext_9->veach_scene_0)->lights_1[min(uint(_S210 * float((&kernelContext_9->veach_scene_0)->light_count_1)), (&kernelContext_9->veach_scene_0)->light_count_1 - 1U)];

#line 127
    float3 _S212 = punctual_irradiance_sample_0(&_S211, p_5, rng_4, l_5, distance_5);

    thread float bsdf_pdf_0;

#line 129
    float3 f_2;
    if(lambert_0)
    {

#line 130
        float3 _S213 = b_9->diffuse_0;

#line 130
        float3 _S214 = Frame_to_local_0(frame_0, *l_5);

#line 130
        float3 _S215 = lambert_eval_0(_S213, _S214, &bsdf_pdf_0);

#line 130
        f_2 = _S215;

#line 130
    }
    else
    {

#line 130
        float3 _S216 = Frame_to_local_0(frame_0, *l_5);

#line 130
        float3 _S217 = brdf_eval_0(b_9, v_local_0, _S216, &bsdf_pdf_0);

#line 130
        f_2 = _S217;

#line 130
    }

    return _S212 * f_2 / float3(select_0) ;
}


#line 362 "engine/render/shaders/veach/scene.slang"
bool light_visible_0(float3 p_6, float3 g_0, float3 l_6, float distance_6, Rng_0 thread* rng_5, KernelContext_0 thread* kernelContext_10)
{

#line 363
    if((((&kernelContext_10->veach_scene_0)->debug_skip_0) & 1U) != 0U)
    {

#line 364
        return true;
    }
    float3 from_1 = offset_origin_0(p_6, g_0, l_6);
    if(distance_6 >= 1.00000001504746622e+29f)
    {

#line 367
        bool _S218 = visible_0(from_1, l_6, distance_6, rng_5, kernelContext_10);
        return _S218;
    }
    float3 to_light_2 = p_6 + l_6 * float3(distance_6)  - from_1;
    float length_to_light_0 = length(to_light_2);

#line 371
    bool _S219 = visible_0(from_1, to_light_2 / float3(length_to_light_0) , length_to_light_0 * 0.99900001287460327f, rng_5, kernelContext_10);
    return _S219;
}


#line 154 "engine/render/shaders/veach/lights.slang"
float3 sample_light_0(float3 p_7, float3 geometric_3, const Frame_0 thread* frame_1, const Brdf_0 thread* b_10, float3 v_local_1, Rng_0 thread* rng_6, bool lambert_1, bool mis_0, KernelContext_0 thread* kernelContext_11)
{
    float pick_0 = Rng_next_0(rng_6);

#line 156
    float _S220 = emissive_selection_probability_0(kernelContext_11);


    thread float3 l_7 = float3(0.0f, 0.0f, 1.0f);
    thread float distance_7 = 0.0f;

#line 160
    bool _S221;

#line 160
    float3 contribution_0;
    if(pick_0 < _S220)
    {
        EmissiveTriangle_natural_0 device* _S222 = (&kernelContext_11->veach_scene_0)->emissive_1;

#line 163
        float _S223 = Rng_next_0(rng_6);

#line 163
        uint _S224 = pick_emissive_0(_S223, kernelContext_11);

#line 163
        EmissiveTriangle_natural_0 tri_0 = _S222[_S224];
        float _S225 = Rng_next_0(rng_6);

#line 164
        float su_0 = sqrt(_S225);
        float _S226 = 1.0f - su_0;

#line 165
        float _S227 = Rng_next_0(rng_6);

#line 165
        TriangleHit_0 _S228 = surface_at_0(tri_0.instance_0, tri_0.primitive_0, float2(_S226, _S227 * su_0), p_7, kernelContext_11);

        float3 to_light_3 = _S228.surface_4.position_0 - p_7;
        float _S229 = length(to_light_3);

#line 168
        distance_7 = _S229;
        float3 _S230 = to_light_3 / float3(_S229) ;

#line 169
        l_7 = _S230;
        float cos_light_2 = dot(_S228.surface_4.geometric_normal_0, - _S230);

#line 170
        thread TriangleHit_0 _S231 = _S228;

#line 170
        float3 _S232 = emission_of_0(&_S231);

#line 170
        thread MaterialRecord_0 _S233 = _S228.material_0;

#line 170
        float _S234 = emissive_light_pdf_0(&_S233, _S228.area_0, _S229, cos_light_2, kernelContext_11);


        if(_S234 <= 0.0f)
        {

#line 173
            _S221 = true;

#line 173
        }
        else
        {

#line 173
            _S221 = all(_S232 <= (float3(0.0f) ));

#line 173
        }

#line 173
        if(_S221)
        {

#line 174
            return float3(0.0f, 0.0f, 0.0f);
        }
        thread float bsdf_pdf_1;
        if(lambert_1)
        {

#line 177
            float3 _S235 = b_10->diffuse_0;

#line 177
            float3 _S236 = Frame_to_local_0(frame_1, l_7);

#line 177
            float3 _S237 = lambert_eval_0(_S235, _S236, &bsdf_pdf_1);

#line 177
            contribution_0 = _S237;

#line 177
        }
        else
        {

#line 177
            float3 _S238 = Frame_to_local_0(frame_1, l_7);

#line 177
            float3 _S239 = brdf_eval_0(b_10, v_local_1, _S238, &bsdf_pdf_1);

#line 177
            contribution_0 = _S239;

#line 177
        }

        float3 _S240 = _S232 * contribution_0;

#line 179
        float _S241;

#line 179
        if(mis_0)
        {

#line 179
            _S241 = power_heuristic_0(_S234, bsdf_pdf_1);

#line 179
        }
        else
        {

#line 179
            _S241 = 1.0f;

#line 179
        }

#line 179
        contribution_0 = _S240 * float3(_S241)  / float3(_S234) ;

#line 161
    }
    else
    {

#line 161
        float3 _S242 = sample_punctual_0(p_7, frame_1, b_10, v_local_1, 1.0f - _S220, rng_6, &l_7, &distance_7, lambert_1, kernelContext_11);

#line 161
        contribution_0 = _S242;

#line 161
    }

#line 183
    if(all(contribution_0 <= (float3(0.0f) )))
    {

#line 183
        _S221 = true;

#line 183
    }
    else
    {

#line 183
        bool _S243 = light_visible_0(p_7, geometric_3, l_7, distance_7, rng_6, kernelContext_11);

#line 183
        _S221 = !_S243;

#line 183
    }

#line 183
    if(_S221)
    {

#line 184
        return float3(0.0f, 0.0f, 0.0f);
    }
    return contribution_0;
}


#line 61 "engine/render/shaders/veach/diffuse_path.slang"
float3 cosine_direction_0(const Frame_0 thread* frame_2, Rng_0 thread* rng_7, float thread* pdf_2)
{

#line 62
    float _S244 = Rng_next_0(rng_7);

#line 62
    float _S245 = Rng_next_0(rng_7);
    float r_2 = sqrt(_S244);
    float _S246 = 6.2831854820251465f * _S245;

#line 64
    float _S247 = sqrt(1.0f - _S244);

#line 64
    float3 l_8 = float3(r_2 * cos(_S246), r_2 * sin(_S246), _S247);
    *pdf_2 = _S247 / 3.1415927410125732f;

#line 65
    float3 _S248 = Frame_to_world_0(frame_2, l_8);
    return _S248;
}


#line 17
struct DiffuseVertex_0
{
    float3 position_4;
    float3 geometric_4;
    float3 normal_9;
    float3 albedo_1;
    float3 emission_1;
    bool lit_0;
};


DiffuseVertex_0 diffuse_vertex_0(const Trace_0 thread* t_7, float3 origin_2, float3 direction_4, float previous_pdf_0, KernelContext_0 thread* kernelContext_12)
{

#line 28
    TriangleHit_0 _S249 = surface_at_0(t_7->instance_1, t_7->primitive_2, t_7->bary_1, origin_2, kernelContext_12);

#line 28
    thread SurfaceInput_0 _S250 = _S249.surface_4;

#line 28
    thread MaterialRecord_0 _S251 = _S249.material_0;

#line 28
    MaterialContext_0 _S252 = MaterialContext_x24init_1(&_S250, &_S251);

#line 28
    thread MaterialContext_0 _S253 = _S252;

#line 28
    SurfaceSample_0 _S254 = evaluateSurface_0(_S249.material_0.kind_0, &_S253);



    thread DiffuseVertex_0 v_6;
    (&v_6)->position_4 = _S249.surface_4.position_0;
    (&v_6)->geometric_4 = _S249.surface_4.geometric_normal_0;
    (&v_6)->normal_9 = _S254.normal_1;
    (&v_6)->albedo_1 = _S254.base_color_1 * float3((1.0f - _S254.metallic_0)) ;
    float3 _S255 = - direction_4;

#line 37
    (&v_6)->lit_0 = (dot(_S254.normal_1, _S255)) > 0.0f;
    (&v_6)->emission_1 = float3(0.0f, 0.0f, 0.0f);

#line 38
    bool _S256;
    if(any((_S254.emission_0) > (float3(0.0f) )))
    {

#line 39
        if(_S249.front_0)
        {

#line 39
            _S256 = true;

#line 39
        }
        else
        {

#line 39
            _S256 = ((_S249.material_0.flags_0) & 64U) != 0U;

#line 39
        }

#line 39
    }
    else
    {

#line 39
        _S256 = false;

#line 39
    }

#line 39
    if(_S256)
    {
        if(previous_pdf_0 > 0.0f)
        {

#line 41
            _S256 = ((_S249.material_0.flags_0) & 128U) != 0U;

#line 41
        }
        else
        {

#line 41
            _S256 = false;

#line 41
        }

#line 41
        float weight_0;

#line 41
        if(_S256)
        {

#line 42
            float _S257 = dot((&v_6)->geometric_4, _S255);

#line 42
            thread MaterialRecord_0 _S258 = _S249.material_0;

#line 42
            float _S259 = emissive_light_pdf_0(&_S258, _S249.area_0, t_7->t_2, _S257, kernelContext_12);

#line 42
            weight_0 = power_heuristic_0(previous_pdf_0, _S259);

#line 41
        }
        else
        {

#line 41
            weight_0 = 1.0f;

#line 41
        }



        (&v_6)->emission_1 = _S254.emission_0 * float3(weight_0) ;

#line 39
    }

#line 47
    return v_6;
}


float3 diffuse_light_0(const DiffuseVertex_0 thread* v_7, Rng_0 thread* rng_8, KernelContext_0 thread* kernelContext_13)
{

#line 52
    thread Brdf_0 b_11;
    (&b_11)->diffuse_0 = v_7->albedo_1;
    (&b_11)->f0_0 = float3(0.0f, 0.0f, 0.0f);
    (&b_11)->alpha_2 = 1.0f;
    (&b_11)->specular_probability_0 = 0.0f;
    float3 _S260 = float3(0.0f, 0.0f, 1.0f);

#line 57
    thread Frame_0 _S261 = frame_of_0(v_7->normal_9);

#line 57
    thread Brdf_0 _S262 = b_11;

#line 57
    float3 _S263 = sample_light_0(v_7->position_4, v_7->geometric_4, &_S261, &_S262, _S260, rng_8, true, true, kernelContext_13);

#line 57
    return _S263;
}


#line 115 "engine/render/shaders/veach/brdf.slang"
float3 sample_vndf_0(float3 v_8, float alpha_3, float2 u_2)
{

#line 116
    float3 vh_0 = normalize(float3(alpha_3 * v_8.x, alpha_3 * v_8.y, v_8.z));
    float _S264 = vh_0.x;

#line 117
    float _S265 = vh_0.y;

#line 117
    float lensq_0 = _S264 * _S264 + _S265 * _S265;

#line 117
    float3 t1_1;
    if(lensq_0 > 0.0f)
    {

#line 118
        t1_1 = float3(- _S265, _S264, 0.0f) / float3(sqrt(lensq_0)) ;

#line 118
    }
    else
    {

#line 118
        t1_1 = float3(1.0f, 0.0f, 0.0f);

#line 118
    }

    float r_3 = sqrt(u_2.x);
    float phi_0 = 6.2831854820251465f * u_2.y;
    float p1_1 = r_3 * cos(phi_0);
    float s_6 = 0.5f * (1.0f + vh_0.z);
    float _S266 = 1.0f - p1_1 * p1_1;

#line 124
    float p2_1 = (1.0f - s_6) * sqrt(_S266) + s_6 * r_3 * sin(phi_0);
    float3 nh_0 = float3(p1_1)  * t1_1 + float3(p2_1)  * cross(vh_0, t1_1) + float3(sqrt(max(0.0f, _S266 - p2_1 * p2_1)))  * vh_0;
    return normalize(float3(alpha_3 * nh_0.x, alpha_3 * nh_0.y, max(0.0f, nh_0.z)));
}

float3 brdf_sample_0(const Brdf_0 thread* b_12, float3 v_9, Rng_0 thread* rng_9)
{

#line 130
    float _S267 = Rng_next_0(rng_9);

#line 130
    float _S268 = Rng_next_0(rng_9);

#line 130
    float2 u_3 = float2(_S267, _S268);
    float _S269 = Rng_next_0(rng_9);

#line 131
    if(_S269 < (b_12->specular_probability_0))
    {

#line 132
        return reflect(- v_9, sample_vndf_0(v_9, b_12->alpha_2, u_3));
    }
    float r_4 = sqrt(_S267);
    float phi_1 = 6.2831854820251465f * _S268;
    return float3(r_4 * cos(phi_1), r_4 * sin(phi_1), sqrt(max(0.0f, 1.0f - _S267)));
}


#line 136
float2 sky_view_uv_0(SkyFrame_natural_0 device* _S270, int _S271, float3 _S272)
{

#line 136
    SkyFrame_natural_0 device* _S273 = _S270+_S271;

#line 138 "engine/render/shaders/veach/atmosphere.slang"
    float _S274 = (float4(_S273->atmosphere_0.planet_0) ).x;

#line 138
    float r_5 = _S274 + (float4(_S273->camera_0) ).x;


    float horizon_elevation_0 = 1.5707963705062866f - acos(clamp(- sqrt(max(r_5 * r_5 - _S274 * _S274, 0.0f)) / r_5, -1.0f, 1.0f));
    float elevation_0 = 1.5707963705062866f - acos(clamp(_S272.y, -1.0f, 1.0f));

#line 142
    float v_10;

    if(elevation_0 >= horizon_elevation_0)
    {

#line 144
        v_10 = 0.5f + 0.5f * sqrt(saturate((elevation_0 - horizon_elevation_0) / (1.5707963705062866f - horizon_elevation_0)));

#line 144
    }
    else
    {

#line 144
        v_10 = 0.5f - 0.5f * sqrt(saturate((horizon_elevation_0 - elevation_0) / (horizon_elevation_0 + 1.5707963705062866f)));

#line 144
    }

#line 144
    float4 _S275 = float4(_S273->sun_0) ;

#line 153
    float delta_0 = atan2(_S272.z, _S272.x) - atan2(_S275.z, _S275.x);

    return float2(0.5f + (delta_0 - 6.2831854820251465f * floor((delta_0 + 3.1415927410125732f) / 6.2831854820251465f)) / 6.2831854820251465f, v_10);
}


#line 155
float3 sample_transmittance_0(SkyFrame_natural_0 device* _S276, int _S277, float _S278, float _S279)
{

#line 155
    SkyFrame_natural_0 device* _S280 = _S276+_S277;

#line 131
    texture2d<float, access::sample> _S281 = _S280->transmittance_0;

#line 131
    sampler _S282 = _S280->linear_clamp_0;

#line 131
    thread AtmosphereParams_natural_0 _S283 = _S280->atmosphere_0;

#line 131
    float2 _S284 = transmittance_uv_0(&_S283, _S278, _S279);

#line 131
    return ((_S281).sample((_S282), (_S284), level((0.0f)))).xyz;
}


#line 131
float3 sky_radiance_0(SkyFrame_natural_0 device* _S285, int _S286, float3 _S287, bool _S288)
{

#line 131
    SkyFrame_natural_0 device* _S289 = _S285+_S286;

#line 131
    uint _S290 = _S289->mode_0;

#line 182
    if((_S289->mode_0) == 0U)
    {

#line 183
        return float3(0.0f, 0.0f, 0.0f);
    }
    if(_S290 == 1U)
    {

#line 186
        return (float4(_S289->constant_0) ).xyz;
    }
    texture2d<float, access::sample> _S291 = _S289->sky_view_0;

#line 188
    sampler _S292 = _S289->linear_clamp_0;

#line 188
    float2 _S293 = sky_view_uv_0(_S285, _S286, _S287);

#line 188
    float3 l_9 = ((_S291).sample((_S292), (_S293), level((0.0f)))).xyz;

#line 188
    bool _S294;
    if(_S288)
    {

#line 189
        _S294 = (dot(_S287, (float4(_S289->sun_0) ).xyz)) > (cos(0.0046250000596046448f));

#line 189
    }
    else
    {

#line 189
        _S294 = false;

#line 189
    }

#line 189
    float3 l_10;

#line 189
    if(_S294)
    {

#line 190
        float _S295 = (float4(_S289->atmosphere_0.planet_0) ).x;

#line 190
        float r_6 = _S295 + (float4(_S289->camera_0) ).x;
        float solid_angle_0 = 6.2831854820251465f * (1.0f - cos(0.0046250000596046448f));

        if((ray_sphere_0(float3(0.0f, r_6, 0.0f), _S287, _S295)) < 0.0f)
        {

#line 193
            float3 _S296 = sample_transmittance_0(_S285, _S286, r_6, _S287.y);

#line 193
            l_10 = l_9 + _S296 / float3(solid_angle_0) ;

#line 193
        }
        else
        {

#line 193
            l_10 = l_9;

#line 193
        }

#line 189
    }
    else
    {

#line 189
        l_10 = l_9;

#line 189
    }

#line 197
    return l_10 * float3((float4(_S289->sun_0) ).w) ;
}


#line 197
float3 diffuse_incoming_0(SkyFrame_natural_0 device* _S297, int _S298, float3 _S299, float3 _S300, uint _S301, Rng_0 thread* _S302, KernelContext_0 thread* kernelContext_14)
{

#line 46 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/pt/path_tracer_variant.slang"
    float3 _S303 = float3(0.0f, 0.0f, 0.0f);
    float3 _S304 = float3(1.0f, 1.0f, 1.0f);

#line 47
    float3 _S305 = _S299;

#line 47
    float3 _S306 = _S300;

#line 47
    float previous_pdf_1 = 0.0f;

#line 47
    uint i_0 = 0U;

#line 47
    float3 throughput_0 = _S304;

#line 47
    float3 result_1 = _S303;

    for(;;)
    {

#line 49
        if(i_0 < _S301)
        {
        }
        else
        {

#line 49
            break;
        }

#line 49
        Trace_0 _S307 = trace_0(_S305, _S306, 1.00000001504746622e+30f, _S302, 255U, kernelContext_14);

        if(!_S307.hit_0)
        {

#line 51
            float3 _S308 = sky_radiance_0(_S297, _S298, _S306, false);

#line 51
            result_1 = result_1 + throughput_0 * _S308;

            break;
        }

#line 53
        thread Trace_0 _S309 = _S307;

#line 53
        DiffuseVertex_0 _S310 = diffuse_vertex_0(&_S309, _S305, _S306, previous_pdf_1, kernelContext_14);

#line 53
        float3 result_2;


        if(i_0 > 0U)
        {

#line 56
            result_2 = result_1 + throughput_0 * _S310.emission_1;

#line 56
        }
        else
        {

#line 56
            result_2 = result_1;

#line 56
        }


        if(!_S310.lit_0)
        {

#line 59
            result_1 = result_2;
            break;
        }

#line 60
        thread DiffuseVertex_0 _S311 = _S310;

#line 60
        float3 _S312 = diffuse_light_0(&_S311, _S302, kernelContext_14);

        float3 result_3 = result_2 + throughput_0 * _S312;

#line 62
        thread Frame_0 _S313 = frame_of_0(_S310.normal_9);
        thread float pdf_3;

#line 63
        float3 _S314 = cosine_direction_0(&_S313, _S302, &pdf_3);

        float3 throughput_1 = throughput_0 * _S310.albedo_1;
        float _S315 = pdf_3;
        float3 _S316 = offset_origin_0(_S310.position_4, _S310.geometric_4, _S314);

#line 49
        uint i_1 = i_0 + 1U;

#line 49
        _S305 = _S316;

#line 49
        _S306 = _S314;

#line 49
        previous_pdf_1 = _S315;

#line 49
        i_0 = i_1;

#line 49
        throughput_0 = throughput_1;

#line 49
        result_1 = result_3;

#line 49
    }

#line 69
    return result_1;
}




float3 radiance_1(float3 origin_3, float3 direction_5, Rng_0 thread* rng_10, float thread* first_hit_0, KernelContext_0 thread* kernelContext_15)
{

#line 75
    float3 result_4;
    float3 _S317 = float3(0.0f, 0.0f, 0.0f);
    float3 _S318 = float3(1.0f, 1.0f, 1.0f);

    *first_hit_0 = 0.0f;

#line 79
    bool direct_0;

#line 87
    if((kernelContext_15->params_0->mode_1) == 1U)
    {

#line 87
        direct_0 = true;

#line 87
    }
    else
    {

#line 87
        direct_0 = (kernelContext_15->params_0->mode_1) == 3U;

#line 87
    }
    bool _S319 = (kernelContext_15->params_0->mode_1) == 4U;

#line 88
    uint _S320;
    if(direct_0)
    {

#line 89
        _S320 = 1U;

#line 89
    }
    else
    {

#line 89
        _S320 = uint(kernelContext_15->params_0->sky_bounces_0.w);

#line 89
    }
    SkyFrame_natural_0 device* _S321 = kernelContext_15->params_0->sky_0;

#line 90
    float3 _S322 = origin_3;

#line 90
    float3 _S323 = direction_5;

#line 90
    bool diffuse_2 = false;

#line 90
    float previous_bsdf_pdf_0 = 0.0f;

#line 90
    uint bounce_0 = 0U;

#line 90
    float3 throughput_2 = _S318;

#line 90
    float3 result_5 = _S317;

    for(;;)
    {

#line 92
        if(bounce_0 <= _S320)
        {
        }
        else
        {

#line 92
            result_4 = result_5;

#line 92
            break;
        }

#line 92
        Trace_0 _S324 = trace_0(_S322, _S323, 1.00000001504746622e+30f, rng_10, 255U, kernelContext_15);

        if(!_S324.hit_0)
        {
            if((kernelContext_15->params_0->mode_1) == 0U)
            {

#line 96
                direct_0 = true;

#line 96
            }
            else
            {

#line 96
                direct_0 = diffuse_2;

#line 96
            }

#line 96
            if(direct_0)
            {

#line 96
                direct_0 = true;

#line 96
            }
            else
            {

#line 96
                if(bounce_0 == 0U)
                {

#line 96
                    if((kernelContext_15->params_0->mode_1) == 3U)
                    {

#line 96
                        direct_0 = true;

#line 96
                    }
                    else
                    {

#line 96
                        direct_0 = _S319;

#line 96
                    }

#line 96
                }
                else
                {

#line 96
                    direct_0 = false;

#line 96
                }

#line 96
            }

#line 96
            if(direct_0)
            {

#line 96
                float3 _S325 = sky_radiance_0(_S321, int(0), _S323, bounce_0 == 0U);

#line 96
                result_4 = result_5 + throughput_2 * _S325;

#line 96
            }
            else
            {

#line 96
                result_4 = result_5;

#line 96
            }


            break;
        }
        bool _S326 = bounce_0 == 0U;

#line 101
        if(_S326)
        {

#line 102
            *first_hit_0 = _S324.t_2;

#line 101
        }

#line 101
        TriangleHit_0 _S327 = surface_at_0(_S324.instance_1, _S324.primitive_2, _S324.bary_1, _S322, kernelContext_15);

#line 101
        thread SurfaceInput_0 _S328 = _S327.surface_4;

#line 101
        thread MaterialRecord_0 _S329 = _S327.material_0;

#line 101
        MaterialContext_0 _S330 = MaterialContext_x24init_1(&_S328, &_S329);

#line 101
        thread MaterialContext_0 _S331 = _S330;

#line 101
        SurfaceSample_0 _S332 = evaluateSurface_0(_S327.material_0.kind_0, &_S331);

#line 101
        float3 _S333 = float3(0.0f) ;

#line 110
        if(any((_S332.emission_0) > _S333))
        {

#line 110
            if(_S327.front_0)
            {

#line 110
                direct_0 = true;

#line 110
            }
            else
            {

#line 110
                direct_0 = ((_S327.material_0.flags_0) & 64U) != 0U;

#line 110
            }

#line 110
        }
        else
        {

#line 110
            direct_0 = false;

#line 110
        }

#line 110
        bool _S334;

#line 110
        if(direct_0)
        {

#line 111
            if(_S319)
            {

#line 111
                _S334 = bounce_0 == 1U;

#line 111
            }
            else
            {

#line 111
                _S334 = false;

#line 111
            }

#line 111
            _S334 = !_S334;

#line 110
        }
        else
        {

#line 110
            _S334 = false;

#line 110
        }

#line 110
        bool _S335;

#line 110
        float3 result_6;

#line 110
        if(_S334)
        {

            if(previous_bsdf_pdf_0 > 0.0f)
            {

#line 113
                _S335 = ((_S327.material_0.flags_0) & 128U) != 0U;

#line 113
            }
            else
            {

#line 113
                _S335 = false;

#line 113
            }

#line 113
            float weight_1;

#line 113
            if(_S335)
            {

#line 114
                float _S336 = dot(_S327.surface_4.geometric_normal_0, - _S323);

#line 114
                thread MaterialRecord_0 _S337 = _S327.material_0;

#line 114
                float _S338 = emissive_light_pdf_0(&_S337, _S327.area_0, _S324.t_2, _S336, kernelContext_15);

#line 114
                weight_1 = power_heuristic_0(previous_bsdf_pdf_0, _S338);

#line 113
            }
            else
            {

#line 113
                weight_1 = 1.0f;

#line 113
            }

#line 113
            result_6 = result_5 + throughput_2 * _S332.emission_0 * float3(weight_1) ;

#line 110
        }
        else
        {

#line 110
            result_6 = result_5;

#line 110
        }

#line 119
        if(bounce_0 == _S320)
        {

#line 119
            result_4 = result_6;
            break;
        }

        Frame_0 frame_3 = frame_of_0(_S332.normal_1);
        if((kernelContext_15->params_0->mode_1) == 2U)
        {

            float _S339 = Rng_next_0(rng_10);

#line 127
            float _S340 = Rng_next_0(rng_10);
            float r_7 = sqrt(_S339);
            float _S341 = 6.2831854820251465f * _S340;

#line 129
            float3 _S342 = float3(r_7 * cos(_S341), r_7 * sin(_S341), sqrt(1.0f - _S339));

#line 129
            thread Frame_0 _S343 = frame_3;

#line 129
            float3 _S344 = Frame_to_world_0(&_S343, _S342);

#line 129
            bool _S345 = visible_0(offset_origin_0(_S327.surface_4.position_0, _S327.surface_4.geometric_normal_0, _S344), _S344, 1.0f, rng_10, kernelContext_15);

            if(_S345)
            {

#line 131
                previous_bsdf_pdf_0 = 1.0f;

#line 131
            }
            else
            {

#line 131
                previous_bsdf_pdf_0 = 0.0f;

#line 131
            }

#line 131
            return float3(previous_bsdf_pdf_0)  / float3(max(kernelContext_15->params_0->camera_position_exposure_0.w, 9.999999960041972e-13f)) ;
        }

#line 131
        thread Frame_0 _S346 = frame_3;

#line 131
        float3 _S347 = Frame_to_local_0(&_S346, _S327.surface_4.view_0);


        float _S348 = _S347.z;

#line 134
        if(_S348 <= 0.0f)
        {

#line 134
            result_4 = result_6;
            break;
        }

#line 135
        thread SurfaceSample_0 _S349 = _S332;

#line 135
        Brdf_0 _S350 = brdf_of_0(&_S349, _S348);

        thread Brdf_0 b_13 = _S350;
        if(diffuse_2)
        {

#line 139
            (&b_13)->f0_0 = _S317;
            (&b_13)->specular_probability_0 = 0.0f;

#line 138
        }



        Brdf_0 _S351 = b_13;
        if(_S319)
        {

#line 143
            _S335 = _S326;

#line 143
        }
        else
        {

#line 143
            _S335 = false;

#line 143
        }

#line 143
        bool _S352 = !_S335;

#line 143
        thread Frame_0 _S353 = frame_3;

#line 143
        thread Brdf_0 _S354 = _S351;

#line 143
        float3 _S355 = sample_light_0(_S327.surface_4.position_0, _S327.surface_4.geometric_normal_0, &_S353, &_S354, _S347, rng_10, diffuse_2, _S352, kernelContext_15);

#line 142
        float3 result_7 = result_6 + throughput_2 * _S355;

#line 142
        float3 result_8;

        if((kernelContext_15->params_0->mode_1) == 5U)
        {

#line 144
            thread Frame_0 _S356 = frame_3;
            thread float pdf5_0;

#line 145
            float3 _S357 = cosine_direction_0(&_S356, rng_10, &pdf5_0);

            float3 _S358 = throughput_2 * (&b_13)->diffuse_0;

#line 147
            float3 _S359 = diffuse_incoming_0(_S321, int(0), offset_origin_0(_S327.surface_4.position_0, _S327.surface_4.geometric_normal_0, _S357), _S357, 8U, rng_10, kernelContext_15);

#line 147
            result_8 = result_7 + _S358 * _S359;

#line 144
        }
        else
        {

#line 144
            result_8 = result_7;

#line 144
        }

#line 149
        if((kernelContext_15->params_0->mode_1) == 3U)
        {

#line 150
            float _S360 = Rng_next_0(rng_10);

#line 150
            float _S361 = Rng_next_0(rng_10);
            float r_8 = sqrt(_S360);
            float _S362 = 6.2831854820251465f * _S361;

#line 152
            float3 _S363 = float3(r_8 * cos(_S362), r_8 * sin(_S362), sqrt(1.0f - _S360));

#line 152
            thread Frame_0 _S364 = frame_3;

#line 152
            float3 _S365 = Frame_to_world_0(&_S364, _S363);

#line 152
            bool _S366 = visible_0(offset_origin_0(_S327.surface_4.position_0, _S327.surface_4.geometric_normal_0, _S365), _S365, 1.00000001504746622e+30f, rng_10, kernelContext_15);
            if(_S366)
            {

#line 154
                float3 _S367 = throughput_2 * (&b_13)->diffuse_0;

#line 154
                float3 _S368 = sky_radiance_0(_S321, int(0), _S365, false);

#line 154
                result_4 = result_8 + _S367 * _S368;

#line 153
            }
            else
            {

#line 153
                result_4 = result_8;

#line 153
            }

#line 149
        }
        else
        {

#line 149
            result_4 = result_8;

#line 149
        }

#line 159
        thread float pdf_4;

#line 159
        float3 f_3;

#line 159
        float3 l_11;

        if(_S319)
        {

#line 161
            thread Frame_0 _S369 = frame_3;

#line 161
            float3 _S370 = cosine_direction_0(&_S369, rng_10, &pdf_4);

#line 161
            thread Frame_0 _S371 = frame_3;

#line 161
            float3 _S372 = Frame_to_local_0(&_S371, _S370);

            float3 _S373 = lambert_eval_0((&b_13)->diffuse_0, _S372, &pdf_4);

#line 163
            f_3 = _S373;

#line 163
            l_11 = _S372;

#line 163
            diffuse_2 = true;

#line 161
        }
        else
        {

#line 161
            thread Brdf_0 _S374 = b_13;

#line 161
            float3 _S375 = brdf_sample_0(&_S374, _S347, rng_10);

#line 161
            thread Brdf_0 _S376 = b_13;

#line 161
            float3 _S377 = brdf_eval_0(&_S376, _S347, _S375, &pdf_4);

#line 161
            f_3 = _S377;

#line 161
            l_11 = _S375;

#line 161
        }

#line 161
        bool _S378;

#line 169
        if(pdf_4 <= 0.0f)
        {

#line 169
            _S378 = true;

#line 169
        }
        else
        {

#line 169
            _S378 = all(f_3 <= _S333);

#line 169
        }

#line 169
        if(_S378)
        {

#line 170
            break;
        }
        float3 throughput_3 = throughput_2 * (f_3 / float3(pdf_4) );
        float _S379 = pdf_4;

#line 173
        thread Frame_0 _S380 = frame_3;

#line 173
        float3 _S381 = Frame_to_world_0(&_S380, l_11);

        float3 _S382 = offset_origin_0(_S327.surface_4.position_0, _S327.surface_4.geometric_normal_0, _S381);

        if(bounce_0 >= 3U)
        {

#line 178
            float survive_0 = clamp(max(throughput_3.x, max(throughput_3.y, throughput_3.z)), 0.05000000074505806f, 0.94999998807907104f);
            float _S383 = Rng_next_0(rng_10);

#line 179
            if(_S383 > survive_0)
            {

#line 180
                break;
            }

#line 180
            throughput_2 = throughput_3 / float3(survive_0) ;

#line 177
        }
        else
        {

#line 177
            throughput_2 = throughput_3;

#line 177
        }

#line 92
        uint bounce_1 = bounce_0 + 1U;

#line 92
        _S322 = _S382;

#line 92
        _S323 = _S381;

#line 92
        previous_bsdf_pdf_0 = _S379;

#line 92
        bounce_0 = bounce_1;

#line 92
        result_5 = result_4;

#line 92
    }

#line 185
    return result_4;
}


#line 212 "engine/render/shaders/veach/atmosphere.slang"
float aerial_slice_0(float depth_km_0)
{

#line 213
    return sqrt(saturate(depth_km_0 / 32.0f));
}


#line 195 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/pt/path_tracer_variant.slang"
bool finite_0(float3 v_11)
{

#line 195
    uint3 _S384 = uint3(2139095040U) ;

    return all(((as_type<uint3>((v_11))) & _S384) != _S384);
}


#line 197
float4 aerial_perspective_0(SkyFrame_natural_0 device* _S385, int _S386, float2 _S387, float _S388)
{

#line 197
    SkyFrame_natural_0 device* _S389 = _S385+_S386;

#line 197
    bool _S390;

#line 219 "engine/render/shaders/veach/atmosphere.slang"
    if((_S389->mode_0) != 2U)
    {

#line 219
        _S390 = true;

#line 219
    }
    else
    {

#line 219
        _S390 = (_S389->aerial_on_0) == 0U;

#line 219
    }

#line 219
    if(_S390)
    {

#line 220
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    float slice_0 = aerial_slice_0(_S388 * 0.0010000000474974513f) * 32.0f - 0.5f;
    float lo_1 = clamp(floor(slice_0), 0.0f, 31.0f);


    float2 cell_0 = clamp(_S387, float2(0.015625f) , float2(0.984375f) );
    float _S391 = cell_0.x;

#line 227
    float _S392 = cell_0.y;

    float4 v_12 = mix(((_S389->aerial_0).sample((_S389->linear_clamp_0), (float2((lo_1 + _S391) / 32.0f, _S392)), level((0.0f)))), ((_S389->aerial_0).sample((_S389->linear_clamp_0), (float2((min(lo_1 + 1.0f, 31.0f) + _S391) / 32.0f, _S392)), level((0.0f)))), float4(saturate(slice_0 - lo_1)) );

#line 229
    float4 v_13;
    if(slice_0 < 0.0f)
    {

#line 230
        v_13 = mix(float4(0.0f, 0.0f, 0.0f, 1.0f), v_12, float4(saturate((slice_0 + 0.5f) / 0.5f)) );

#line 230
    }
    else
    {

#line 230
        v_13 = v_12;

#line 230
    }



    return float4(v_13.xyz * float3((float4(_S389->sun_0) ).w) , v_13.w);
}


#line 202 "/private/tmp/claude-501/-Users-patrickdiascamargo-dev-cpp-veach/dc5e6871-c5be-4993-8591-d8fb0247b3a6/scratchpad/mr/pt/path_tracer_variant.slang"
[[kernel]] void trace_pixels(uint3 id_0 [[thread_position_in_grid]], Params_natural_0 constant* params_1 [[buffer(0)]])
{

#line 202
    float3 l_12;

#line 202
    bool _S393;

#line 202
    thread KernelContext_0 kernelContext_16;

#line 202
    (&kernelContext_16)->params_0 = params_1;
    uint2 _S394 = id_0.xy;

#line 203
    if(any(_S394 >= (params_1->size_1)))
    {

#line 204
        return;
    }
    (&(&kernelContext_16)->veach_scene_0)->tlas_0 = (&kernelContext_16)->params_0->scene_0;
    (&(&kernelContext_16)->veach_scene_0)->instances_1 = (&kernelContext_16)->params_0->instances_0;
    (&(&kernelContext_16)->veach_scene_0)->materials_1 = (&kernelContext_16)->params_0->materials_0;
    (&(&kernelContext_16)->veach_scene_0)->lights_1 = (&kernelContext_16)->params_0->lights_0;
    (&(&kernelContext_16)->veach_scene_0)->emissive_1 = (&kernelContext_16)->params_0->emissive_0;
    (&(&kernelContext_16)->veach_scene_0)->light_count_1 = (&kernelContext_16)->params_0->light_count_0;
    (&(&kernelContext_16)->veach_scene_0)->emissive_count_1 = (&kernelContext_16)->params_0->emissive_count_0;
    (&(&kernelContext_16)->veach_scene_0)->emissive_total_weight_1 = (&kernelContext_16)->params_0->emissive_total_weight_0;
    (&(&kernelContext_16)->veach_scene_0)->debug_skip_0 = 0U;
    (&(&kernelContext_16)->veach_scene_0)->flags_2 = (&kernelContext_16)->params_0->scene_flags_0;
    float3 _S395 = float3(0.0f, 0.0f, 0.0f);

#line 216
    uint k_1 = 0U;

#line 216
    float3 sum_0 = _S395;

#line 216
    uint nonfinite_0 = 0U;

    for(;;)
    {

#line 218
        if(k_1 < ((&kernelContext_16)->params_0->samples_per_frame_0))
        {
        }
        else
        {

#line 218
            break;
        }
        uint _S396 = id_0.x;

#line 220
        uint _S397 = id_0.y;

#line 220
        thread Rng_0 rng_11 = Rng_x24init_0(pcg_0(_S396 + pcg_0(_S397 + pcg_0((&kernelContext_16)->params_0->sample_index_0 + k_1))));
        float _S398 = Rng_next_0(&rng_11);

#line 221
        float _S399 = Rng_next_0(&rng_11);

#line 221
        float2 u_4 = float2(_S398, _S399);

#line 221
        float2 jitter_1;
        if(((&kernelContext_16)->params_0->jitter_0) != 0U)
        {

#line 222
            jitter_1 = u_4;

#line 222
        }
        else
        {

#line 222
            jitter_1 = float2(0.5f, 0.5f);

#line 222
        }
        float2 ndc_1 = float2((float(_S396) + jitter_1.x) / float(params_1->size_1.x) * 2.0f - 1.0f, 1.0f - (float(_S397) + jitter_1.y) / float(params_1->size_1.y) * 2.0f);

#line 223
        float3 _S400 = unproject_0(ndc_1, 1.0f, &kernelContext_16);

#line 223
        float3 _S401 = unproject_0(ndc_1, 0.5f, &kernelContext_16);



        thread float first_hit_1;

#line 227
        float3 _S402 = radiance_1((&kernelContext_16)->params_0->camera_position_exposure_0.xyz, normalize(_S401 - _S400), &rng_11, &first_hit_1, &kernelContext_16);

        if(first_hit_1 > 0.0f)
        {

#line 229
            _S393 = ((&kernelContext_16)->params_0->mode_1) != 1U;

#line 229
        }
        else
        {

#line 229
            _S393 = false;

#line 229
        }

#line 229
        bool _S403;

#line 229
        if(_S393)
        {

#line 229
            _S403 = ((&kernelContext_16)->params_0->mode_1) != 2U;

#line 229
        }
        else
        {

#line 229
            _S403 = false;

#line 229
        }

#line 229
        if(_S403)
        {

#line 229
            float4 _S404 = aerial_perspective_0((&kernelContext_16)->params_0->sky_0, int(0), (float2(_S394) + jitter_1) / float2(params_1->size_1), first_hit_1);

#line 229
            l_12 = _S402 * float3(_S404.w)  + _S404.xyz;

#line 229
        }
        else
        {

#line 229
            l_12 = _S402;

#line 229
        }

#line 234
        if(finite_0(l_12))
        {

#line 234
            sum_0 = sum_0 + l_12;

#line 234
        }
        else
        {

#line 234
            nonfinite_0 = nonfinite_0 + 1U;

#line 234
        }

#line 218
        k_1 = k_1 + 1U;

#line 218
    }

#line 240
    float n_3 = float((&kernelContext_16)->params_0->sample_index_0);
    float k_2 = float((&kernelContext_16)->params_0->samples_per_frame_0);

#line 241
    uint lost_0;


    if(((&kernelContext_16)->params_0->sample_index_0) > 0U)
    {

        float4 _S405 = (((&kernelContext_16)->params_0->accumulation_0).read(vec<uint,2>(((int2(_S394))).xy)));
        if((_S405.w) != n_3)
        {

#line 248
            _S393 = true;

#line 248
        }
        else
        {

#line 248
            _S393 = !finite_0(_S405.xyz);

#line 248
        }

#line 248
        if(_S393)
        {

#line 248
            _S393 = true;

#line 248
        }
        else
        {

#line 248
            _S393 = any((_S405.xyz) < (float3(0.0f) ));

#line 248
        }

#line 248
        int _S406;

#line 248
        if(_S393)
        {

#line 248
            _S406 = int(1);

#line 248
        }
        else
        {

#line 248
            _S406 = int(0);

#line 248
        }
        float3 _S407 = _S405.xyz;

#line 249
        lost_0 = uint(_S406);

#line 249
        l_12 = _S407;

#line 244
    }
    else
    {

#line 244
        lost_0 = 0U;

#line 244
        l_12 = _S395;

#line 244
    }

#line 251
    if(lost_0 != 0U)
    {

#line 252
        uint _S408 = atomic_fetch_add_explicit(((atomic_uint device*)((&kernelContext_16)->params_0->diagnostics_0+int(0))), lost_0, memory_order_relaxed);

#line 251
    }


    if(nonfinite_0 != 0U)
    {

#line 255
        uint _S409 = atomic_fetch_add_explicit(((atomic_uint device*)((&kernelContext_16)->params_0->diagnostics_0+int(1))), nonfinite_0, memory_order_relaxed);

#line 254
    }


    float _S410 = n_3 + k_2;
    texture2d<float, access::read_write> _S411 = (&kernelContext_16)->params_0->accumulation_0;

#line 258
    float4 _S412 = float4((l_12 * float3(n_3)  + sum_0) / float3(_S410) , _S410);

#line 258
    _slang_textureFence(_S411);

#line 258
    _S411.write(_S412,_S394);

#line 258
    _slang_textureFence(_S411);
    return;
}

