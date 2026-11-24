# Build options. Every option is documented in docs/guides/building.md.

option(VEACH_BUILD_TESTS "Build the test suite" ON)
option(VEACH_WARNINGS_AS_ERRORS "Treat warnings as errors on Veach targets" ON)
option(VEACH_TRACY "Enable Tracy profiling zones (fetches the Tracy client)" OFF)
option(VEACH_USE_CCACHE "Use ccache as the compiler launcher when it is installed" ON)
option(VEACH_USE_PCH "Use precompiled headers for Flecs- and Luau-heavy targets" ON)
option(VEACH_SDL "Build the SDL3 platform backend (headless is always built)" ON)
set(VEACH_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address;undefined' (Clang/GCC only)")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
