veach_add_engine_library(render
    SOURCES
        src/atmosphere.cpp
        src/backdrop.cpp
        src/mesh_store.cpp
        src/realtime_renderer.cpp
        src/scene_renderer.cpp
        src/png.cpp
        src/readback.cpp
        src/render_graph.cpp
        src/upload.cpp
        src/renderer.cpp
    PUBLIC_DEPS veach::ecs veach::rhi veach::asset
    PRIVATE_DEPS veach_stb
    PCH <flecs.h>)

veach_add_shader(veach_render shaders/present.slang ENTRIES vs_main:vertex fs_main:fragment)
veach_add_shader(veach_render shaders/display.slang ENTRIES display:compute)
# Spike SP-14 (VCH-090): auto-exposure (realtime_renderer.cpp).
veach_add_shader(veach_render shaders/exposure.slang ENTRIES exposure_histogram:compute exposure_resolve:compute)
# Photo mode's path tracer with StandardMaterial only. Apps with their own materials compile their own copy
# (veach_add_scene_shaders); this one also anchors the C++ parameter layout check (scene_renderer.cpp).
veach_add_shader(veach_render shaders/path_tracer.slang ENTRIES trace_pixels:compute
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_add_shader(veach_render shaders/realtime_visibility.slang ENTRIES vis_vs:vertex vis_fs:fragment
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_add_shader(veach_render shaders/realtime_shade.slang ENTRIES shade:compute
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_add_shader(veach_render shaders/atmosphere.slang ENTRIES transmittance_lut:compute multiscattering_lut:compute
    sky_view_lut:compute aerial_lut:compute sky_irradiance:compute)
# The space backdrop's bake and its composite on the real-time image (ADR 0091, backdrop.cpp).
veach_add_shader(veach_render shaders/backdrop.slang ENTRIES backdrop_bake:compute)
veach_add_shader(veach_render shaders/backdrop_composite.slang ENTRIES backdrop_composite:compute)
veach_shader_library(veach_render NAMESPACE veach_render_shaders)
# png.cpp compiles stb's implementation; its warnings are not ours.
set_source_files_properties(src/png.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
