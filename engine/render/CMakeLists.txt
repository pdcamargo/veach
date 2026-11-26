veach_add_engine_library(render
    SOURCES
        src/atmosphere.cpp
        src/frame_budget.cpp
        src/realtime_renderer.cpp
        src/scene_renderer.cpp
        src/png.cpp
        src/render_graph.cpp
        src/renderer.cpp
    PUBLIC_DEPS veach::ecs veach::rhi veach::asset
    PRIVATE_DEPS veach_stb
    PCH <flecs.h>)

veach_add_shader(veach_render shaders/present.slang ENTRIES vs_main:vertex fs_main:fragment)
veach_add_shader(veach_render shaders/display.slang ENTRIES display:compute)
# The reference path tracer with StandardMaterial only. Apps with their own materials compile their own copy
# (veach_add_scene_shaders); this one also anchors the C++ parameter layout check (scene_renderer.cpp).
veach_add_shader(veach_render shaders/path_tracer.slang ENTRIES trace_pixels:compute
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_add_shader(veach_render shaders/realtime_visibility.slang ENTRIES vis_vs:vertex vis_fs:fragment
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_add_shader(veach_render shaders/realtime_shade.slang ENTRIES surface:compute restir_temporal:compute restir_spatial:compute gi_ambient:compute shade:compute
        accumulate:compute denoise_temporal:compute denoise_variance:compute denoise_atrous:compute
        denoise_combine:compute
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_add_shader(veach_render shaders/candela.slang
    ENTRIES cache_update:compute cache_resolve:compute gather:compute misses:compute temporal:compute spatial:compute surface:compute
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_add_shader(veach_render shaders/taau.slang ENTRIES taau:compute)
veach_add_shader(veach_render shaders/atmosphere.slang ENTRIES transmittance_lut:compute multiscattering_lut:compute
    sky_view_lut:compute aerial_lut:compute sky_irradiance:compute)
veach_shader_library(veach_render NAMESPACE veach_render_shaders)
# png.cpp compiles stb's implementation; its warnings are not ours.
set_source_files_properties(src/png.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
