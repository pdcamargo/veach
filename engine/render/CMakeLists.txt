veach_add_engine_library(render
    SOURCES
        src/path_tracer.cpp
        src/png.cpp
        src/render_graph.cpp
        src/renderer.cpp
    PUBLIC_DEPS veach::ecs veach::rhi veach::asset
    PRIVATE_DEPS veach_stb
    PCH <flecs.h>)

veach_add_shader(veach_render shaders/present.slang ENTRIES vs_main:vertex fs_main:fragment)
veach_add_shader(veach_render shaders/display.slang ENTRIES display:compute)
# The reference path tracer with StandardMaterial only. Apps with their own materials compile their own copy
# (veach_add_path_tracer); this one also anchors the C++ parameter layout check (path_tracer.cpp).
veach_add_shader(veach_render shaders/path_tracer.slang ENTRIES trace_pixels:compute
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/default_materials")
veach_shader_library(veach_render NAMESPACE veach_render_shaders)
# png.cpp compiles stb's implementation; its warnings are not ours.
set_source_files_properties(src/png.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
