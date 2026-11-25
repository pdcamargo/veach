veach_add_engine_library(asset
    SOURCES
        src/asset_server.cpp
        src/gltf.cpp
        src/meshlets.cpp
        src/stb_image.cpp
        src/texture.cpp
    PUBLIC_DEPS veach::ecs veach::platform
    PRIVATE_DEPS veach_stb veach_bc7enc fastgltf::fastgltf meshoptimizer
    PCH <flecs.h> <nlohmann/json.hpp>)
# stb's and bc7enc's headers are not ours to warn about.
set_source_files_properties(src/stb_image.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
