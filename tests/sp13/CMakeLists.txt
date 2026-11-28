# Spike SP-13 (VCH-089), throwaway: walk, hover and land on a rotating planet (docs/research/spikes/sp-13-walk-hover-land.md).
# The physics runs on engine/physics (VCH-113); the scenario, flight model and autopilot stay here until VCH-115.
add_library(veach-sp13 STATIC sp13_sim.cpp sp13_module.cpp)
target_link_libraries(veach-sp13 PUBLIC veach::veach veach::space)
target_include_directories(veach-sp13 PUBLIC "${PROJECT_SOURCE_DIR}/tests")
set_target_properties(veach-sp13 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-sp13)

target_sources(veach-tests PRIVATE sp13_tests.cpp)
target_link_libraries(veach-tests PRIVATE veach-sp13)

# Drift over hours, |Δv| at every transfer, ms per step: veach-bench-sp13 [--hours H] [--scale S] (run by hand, Release).
add_executable(veach-bench-sp13 sp13_bench.cpp)
target_link_libraries(veach-bench-sp13 PRIVATE veach-sp13)
set_target_properties(veach-bench-sp13 PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-bench-sp13)

# A look at the scene in the lab (VEACH_SP13=s, apps/lab/src/sp13_spike.hpp). Added from here because the spike library
# lives with the tests: the lab builds without it when tests are off.
if(TARGET veach-lab)
    target_sources(veach-lab PRIVATE "${PROJECT_SOURCE_DIR}/apps/lab/src/sp13_spike.cpp")
    target_compile_definitions(veach-lab PRIVATE VEACH_LAB_SP13=1)
    target_link_libraries(veach-lab PRIVATE veach-sp13)
endif()
