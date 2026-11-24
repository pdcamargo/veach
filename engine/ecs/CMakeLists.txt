veach_add_engine_library(ecs
    SOURCES
        src/command_buffer.cpp
        src/core_module.cpp
        src/json.cpp
        src/phases.cpp
        src/reflection.cpp
        src/services.cpp
    PUBLIC_DEPS veach::core flecs::flecs_static nlohmann_json::nlohmann_json
    PCH <flecs.h> <nlohmann/json.hpp>)
