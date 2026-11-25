veach_add_engine_library(runtime
    SOURCES
        src/config.cpp
        src/engine.cpp
        src/mcp_server.cpp
    PUBLIC_DEPS veach::ecs veach::platform veach::asset veach::script veach::rhi veach::render
    PCH <flecs.h> <nlohmann/json.hpp>)
