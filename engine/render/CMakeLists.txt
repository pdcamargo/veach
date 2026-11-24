veach_add_engine_library(render
    SOURCES src/renderer.cpp
    PUBLIC_DEPS veach::ecs veach::rhi
    PCH <flecs.h>)
