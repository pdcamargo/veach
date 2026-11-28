veach_add_app(veach-lab
    SOURCES
        src/lab_module.cpp
        src/main.cpp
        src/runtime_mesh_demo.cpp
        src/sp03_spike.cpp
        src/sp04_spike.cpp
        src/sp10_spike.cpp
        src/sp14_spike.cpp
        src/triangle_feature.cpp
    DEPS veach::space)  # the space toolbox (ADR 0042); first used by spike SP-04

# The lab finds its project (veach.project.json, scripts/) from any working directory.
target_compile_definitions(veach-lab PRIVATE VEACH_LAB_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

veach_add_shader(veach-lab shaders/triangle.slang ENTRIES vs_main:vertex fs_main:fragment)
veach_add_shader(veach-lab shaders/triangle_rt.slang ENTRIES trace:compute)
# The material model's proof (ADR 0035): GlowMaterial overrides only evalEmission.
veach_add_material(veach-lab GlowMaterial shaders/glow_material.slang EMISSIVE)
veach_add_scene_shaders(veach-lab NAMESPACE lab_shaders)
veach_shader_library(veach-lab NAMESPACE lab_shaders)
