# The space toolbox (docs/adr/0042-space-toolbox-layering.md): a layer on top of the engine. It links the engine like
# any game does; the engine never links it, and tools/scripts/check_repo.py fails if engine/ names it.
#
# First sources: spike SP-04 (VCH-080, docs/research/spikes/sp-04-orrery-clock.md), the universe clock and bodies on
# rails (ADR 0047, 0048, 0049). Canonical files carry a `// veach: canonical` header (ADR 0043).
add_library(veach_space STATIC
    src/clock.cpp
    src/orbit.cpp
    src/orrery.cpp
    src/presets.cpp
    src/sun.cpp)
add_library(veach::space ALIAS veach_space)
target_include_directories(veach_space PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(veach_space PUBLIC veach::veach)
set_target_properties(veach_space PROPERTIES FOLDER "space")
veach_apply_warnings(veach_space)
