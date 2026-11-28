# Generation stages S1 and B1, addresses and the realization layer: spike SP-05 (VCH-081), throwaway
# (docs/research/spikes/sp-05-system-statistics.md). Canonical code (ADR 0043). The stages run on the engine's stage
# runtime (veach::procgen, VCH-112; stages.hpp), which also gives them their seeds and record hashing.
add_library(veach_space_gen STATIC
    src/address.cpp
    src/atmosphere.cpp  # VCH-115: the stage space:body.atmosphere
    src/realize.cpp
    src/stages.cpp
    src/system.cpp)
target_include_directories(veach_space_gen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(veach_space_gen PUBLIC veach::core veach::procgen)
set_target_properties(veach_space_gen PROPERTIES FOLDER "space")
veach_apply_warnings(veach_space_gen)
