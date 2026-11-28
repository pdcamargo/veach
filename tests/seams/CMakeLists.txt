# Seams sweep, throwaway: terrain, digging and walking at the octant planes, the poles, the cube-corner directions
# and chunk corners (docs/research/spikes/seams-sweep.md). Builds on SP-12 (terrain, colliders, the dig flow) and
# SP-13's character controller, on engine/physics since VCH-113.
add_library(veach-seams STATIC seams.cpp seams_walk.cpp)
target_link_libraries(veach-seams PUBLIC veach-sp12)
target_include_directories(veach-seams PUBLIC "${PROJECT_SOURCE_DIR}/tests")
set_target_properties(veach-seams PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-seams)

target_sources(veach-tests PRIVATE seams_tests.cpp)
target_link_libraries(veach-tests PRIVATE veach-seams)

# Per-site tables, the cube-map checks and the screenshot projects: veach-bench-seams [lod|dig|walk|cubemap] [--out DIR].
add_executable(veach-bench-seams seams_bench.cpp)
target_link_libraries(veach-bench-seams PRIVATE veach-seams veach-sp13 veach-sp06 veach_space_gen)
target_include_directories(veach-bench-seams PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(veach-bench-seams PRIVATE VEACH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(veach-bench-seams PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-bench-seams)
