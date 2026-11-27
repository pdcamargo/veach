veach_add_engine_library(core
    SOURCES
        src/assert.cpp
        src/dmath.cpp
        src/error.cpp
        src/ids.cpp
        src/log.cpp
        src/suggest.cpp
    PUBLIC_DEPS glm::glm)

# veach::dmath's implementation (src/dmath.cpp only): LLVM libc's math headers, pinned in cmake/VeachDependencies.cmake.
target_include_directories(veach_core SYSTEM PRIVATE "${VEACH_LLVM_LIBC_DIR}")

# Conventions from ADR 0013 must be identical in every translation unit.
target_compile_definitions(veach_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_FORCE_XYZW_ONLY)

if(VEACH_TRACY)
    target_link_libraries(veach_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(veach_core PUBLIC VEACH_TRACY)
endif()
