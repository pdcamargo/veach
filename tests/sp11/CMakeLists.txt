# Spike SP-11 (VCH-087), throwaway: Jolt double precision, cross-platform deterministic (docs/research/spikes/sp-11-jolt.md).
# Jolt links PRIVATE: its compile options and definitions stay inside veach-sp11, and sp11_scene.hpp has no Jolt types.
add_library(veach-sp11 STATIC sp11_scene.cpp)
target_link_libraries(veach-sp11 PRIVATE Jolt)
target_include_directories(veach-sp11 PUBLIC "${PROJECT_SOURCE_DIR}/tests")
set_target_properties(veach-sp11 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-sp11)

target_sources(veach-tests PRIVATE sp11_jolt_tests.cpp)
target_link_libraries(veach-tests PRIVATE veach-sp11)

# Hashes and ms per step: veach-bench-sp11 [--bodies N]... [--workers N]... [--steps N] (run by hand, Release).
add_executable(veach-bench-sp11 sp11_jolt_bench.cpp)
target_link_libraries(veach-bench-sp11 PRIVATE veach-sp11)
set_target_properties(veach-bench-sp11 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-bench-sp11)
