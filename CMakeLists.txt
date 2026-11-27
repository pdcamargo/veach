cmake_minimum_required(VERSION 3.29)

project(veach
    VERSION 0.1.0
    DESCRIPTION "AI-native C++ game engine, the foundation of a space-game toolbox"
    LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(VeachOptions)
include(VeachCompiler)
include(VeachDependencies)
include(VeachTargets)
include(VeachShaders)

if(VEACH_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)
add_subdirectory(space)  # the space toolbox: above the engine, never below it (ADR 0042)
add_subdirectory(apps)
add_subdirectory(tools)

if(VEACH_BUILD_TESTS)
    add_subdirectory(tests)
endif()
