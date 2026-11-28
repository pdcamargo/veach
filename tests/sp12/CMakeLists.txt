# Spike SP-12 (VCH-088), throwaway: brush edits on Earth-sized terrain and colliders cooked from the chunks
# (docs/research/spikes/sp-12-dig-collide.md). The physics side runs on engine/physics since VCH-113; the terrain
# region stays here until terrain streaming (VCH-114) gives it an engine home.
add_library(veach-sp12 STATIC sp12_terrain.cpp sp12_scenario.cpp)
target_link_libraries(veach-sp12 PUBLIC veach::voxel veach::physics)
target_include_directories(veach-sp12 PUBLIC "${PROJECT_SOURCE_DIR}/tests")
set_target_properties(veach-sp12 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-sp12)

target_sources(veach-tests PRIVATE sp12_tests.cpp)
target_link_libraries(veach-tests PRIVATE veach-sp12)

# Hashes, ms per chunk and the digging rate: veach-bench-sp12 [--half N] [--ops N] [--out DIR] (by hand, Release).
add_executable(veach-bench-sp12 sp12_bench.cpp)
target_link_libraries(veach-bench-sp12 PRIVATE veach-sp12)
set_target_properties(veach-bench-sp12 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-bench-sp12)
