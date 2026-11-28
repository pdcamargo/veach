# The space toolbox (docs/adr/0042-space-toolbox-layering.md): a layer on top of the engine. It links the engine like
# any game does; the engine never links it, and tools/scripts/check_repo.py fails if engine/ names it. Today it holds
# spike code (docs/research/spikes/) that the real toolbox replaces: SP-04's universe clock and bodies on rails
# (ADR 0047, 0048, 0049) here, SP-05's generation stages in gen/. Canonical files carry a `// veach: canonical` header
# (ADR 0043).
add_library(veach_space STATIC
    src/atmosphere.cpp  # spike SP-10 (VCH-086)
    src/backdrop.cpp      # VCH-131: the system's space backdrop
    src/clock.cpp
    src/flight.cpp        # VCH-115: governor, handoff bands, assisted flight
    src/mcp_tools.cpp
    src/orbit.cpp
    src/orrery.cpp
    src/presets.cpp
    src/realization.cpp   # VCH-115: the realization layer (ADR 0049, 0088)
    src/sun.cpp
    src/terrain.cpp       # VCH-115: terrain fields per body
    src/universe.cpp)     # VCH-115: UniverseModule
add_library(veach::space ALIAS veach_space)
target_include_directories(veach_space PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(veach_space PUBLIC veach::veach)
set_target_properties(veach_space PROPERTIES FOLDER "space")
veach_apply_warnings(veach_space)

add_subdirectory(gen)  # spike SP-05 (VCH-081): generation stages S1 and B1
target_link_libraries(veach_space PUBLIC veach_space_gen)
