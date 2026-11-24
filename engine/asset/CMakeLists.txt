veach_add_engine_library(asset
    SOURCES src/asset_server.cpp
    PUBLIC_DEPS veach::ecs veach::platform
    PCH <flecs.h> <nlohmann/json.hpp>)
