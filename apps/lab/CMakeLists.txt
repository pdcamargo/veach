veach_add_app(veach-lab
    SOURCES
        src/lab_module.cpp
        src/main.cpp
        src/runtime_mesh_demo.cpp
        src/sp04_spike.cpp
        src/terrain_view.cpp
        src/sp10_spike.cpp
        src/sp14_spike.cpp
        src/sp16_spike.cpp
        src/triangle_feature.cpp
    DEPS veach::space veach::voxel)  # the space toolbox (ADR 0042); first used by spike SP-04

# The lab finds its project (veach.project.json, scripts/) from any working directory.
target_compile_definitions(veach-lab PRIVATE VEACH_LAB_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

veach_add_shader(veach-lab shaders/triangle.slang ENTRIES vs_main:vertex fs_main:fragment)
veach_add_shader(veach-lab shaders/triangle_rt.slang ENTRIES trace:compute)
# The material model's proof (ADR 0035): GlowMaterial overrides only evalEmission.
veach_add_material(veach-lab GlowMaterial shaders/glow_material.slang EMISSIVE)
# Spike SP-16 (VCH-092), throwaway: the vegetation field, visibility buffer against forward (src/sp16_spike.cpp).
set(sp16_includes INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
veach_add_shader(veach-lab shaders/sp16_cull.slang
    ENTRIES cull:compute write_args:compute hiz_first:compute hiz_down:compute ${sp16_includes})
veach_add_shader(veach-lab shaders/sp16_raster.slang
    ENTRIES vs_vis:vertex vs_forward:vertex vis_opaque_fs:fragment vis_masked_fs:fragment forward_fs:fragment
            forward_masked_fs:fragment forward_a2c_fs:fragment prepass_fs:fragment prepass_masked_fs:fragment
            prepass_a2c_fs:fragment sky_vs:vertex sky_fs:fragment count_fs:fragment ${sp16_includes})
veach_add_shader(veach-lab shaders/sp16_shade.slang ENTRIES shade:compute display:compute quad_stats:compute ${sp16_includes})
veach_add_scene_shaders(veach-lab NAMESPACE lab_shaders)
veach_shader_library(veach-lab NAMESPACE lab_shaders)
