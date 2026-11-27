# Generation stages S1 and B1, addresses and the realization layer: spike SP-05 (VCH-081), throwaway
# (docs/research/spikes/sp-05-system-statistics.md). Canonical code (ADR 0043); only the engine's core (veach::hash,
# veach::dmath) is needed.
add_library(veach_space_gen STATIC
    src/address.cpp
    src/realize.cpp
    src/system.cpp)
target_include_directories(veach_space_gen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(veach_space_gen PUBLIC veach::core)
set_target_properties(veach_space_gen PROPERTIES FOLDER "space")
veach_apply_warnings(veach_space_gen)
