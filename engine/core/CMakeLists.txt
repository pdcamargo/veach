veach_add_engine_library(core
    SOURCES
        src/assert.cpp
        src/dmath.cpp
        src/env.cpp
        src/error.cpp
        src/ids.cpp
        src/jobs.cpp
        src/log.cpp
        src/suggest.cpp
    PUBLIC_DEPS glm::glm
    PRIVATE_DEPS veach_enkits)

# veach::dmath's implementation (src/dmath.cpp only): LLVM libc's math headers, pinned in cmake/VeachDependencies.cmake.
target_include_directories(veach_core SYSTEM PRIVATE "${VEACH_LLVM_LIBC_DIR}")
# MSVC reports C4702 (unreachable code) inside those headers when it generates code for dmath.cpp; /external:W0 does
# not cover code-generation warnings, so the warning is off for this one file.
if(MSVC)
    set_source_files_properties(src/dmath.cpp PROPERTIES COMPILE_OPTIONS /wd4702)
endif()

# Conventions from ADR 0013 must be identical in every translation unit.
target_compile_definitions(veach_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_FORCE_XYZW_ONLY
    GLM_FORCE_EXPLICIT_CTOR)

if(VEACH_TRACY)
    target_link_libraries(veach_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(veach_core PUBLIC VEACH_TRACY)
endif()
