veach_add_engine_library(render
    SOURCES
        src/png.cpp
        src/render_graph.cpp
        src/renderer.cpp
    PUBLIC_DEPS veach::ecs veach::rhi
    PRIVATE_DEPS veach_stb
    PCH <flecs.h>)

veach_add_shader(veach_render shaders/present.slang ENTRIES vs_main:vertex fs_main:fragment)
veach_shader_library(veach_render NAMESPACE veach_render_shaders)
# png.cpp compiles stb's implementation; its warnings are not ours.
set_source_files_properties(src/png.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
