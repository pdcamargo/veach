veach_add_engine_library(runtime
    SOURCES
        src/config.cpp
        src/engine.cpp
        src/frames_tool.cpp
        src/mcp_server.cpp
        src/mcp_tools.cpp
        src/physics_tools.cpp
        src/replay.cpp
    PUBLIC_DEPS veach::ecs veach::platform veach::asset veach::script veach::rhi veach::render veach::physics
    PCH <flecs.h> <nlohmann/json.hpp>)
