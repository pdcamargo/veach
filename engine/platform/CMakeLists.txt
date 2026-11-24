set(_platform_sources
    src/fs.cpp
    src/headless_platform.cpp
    src/platform.cpp
    src/platform_module.cpp)

if(VEACH_SDL)
    list(APPEND _platform_sources src/sdl_platform.cpp)
endif()

veach_add_engine_library(platform
    SOURCES ${_platform_sources}
    PUBLIC_DEPS veach::ecs
    PCH <flecs.h>)

if(VEACH_SDL)
    # PRIVATE on purpose: SDL types never leave engine/platform (ADR 0008).
    target_link_libraries(veach_platform PRIVATE SDL3::SDL3-static)
    target_compile_definitions(veach_platform PRIVATE VEACH_HAS_SDL)
endif()
