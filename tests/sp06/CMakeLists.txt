# Spike SP-06 (VCH-082), throwaway: one field graph, three evaluators (docs/research/spikes/sp-06-field-graph.md).
# The compiler and the CPU interpreter are engine/procgen now (VCH-112); this keeps the two GPU evaluators, which
# prototype the render feature ADR 0065 puts in engine/render. A static library shared by the unit tests and the
# bench, so the shaders compile once.
add_library(veach-sp06 STATIC field_gpu.cpp)
target_link_libraries(veach-sp06 PUBLIC veach::veach)
target_include_directories(veach-sp06 PUBLIC "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(veach-sp06 PRIVATE VEACH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set(_sp06_shaders "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
veach_add_shader(veach-sp06 shaders/sp06_field_interpreter.slang ENTRIES field_interpret:compute INCLUDES ${_sp06_shaders})
veach_add_shader(veach-sp06 shaders/sp06_field_handwritten.slang ENTRIES field_handwritten:compute
    INCLUDES ${_sp06_shaders})
veach_shader_library(veach-sp06 NAMESPACE sp06_shaders)
set_target_properties(veach-sp06 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-sp06)

target_sources(veach-tests PRIVATE sp06_field_graph_tests.cpp)
target_link_libraries(veach-tests PRIVATE veach-sp06)

# Timings, agreement and the images: veach-bench-sp06 [--resolution N] [--frames N] [--out DIR] (run by hand).
add_executable(veach-bench-sp06 sp06_field_graph_bench.cpp)
target_link_libraries(veach-bench-sp06 PRIVATE veach-sp06)
set_target_properties(veach-bench-sp06 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-bench-sp06)
veach_deploy_d3d12(veach-bench-sp06)
