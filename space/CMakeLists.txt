# The space toolbox (docs/adr/0042-space-toolbox-layering.md): a layer on top of the engine. It links the engine like
# any game does; the engine never links it, and tools/scripts/check_repo.py fails if engine/ names it. Today it holds
# spike code (docs/research/spikes/) that the real toolbox replaces.
add_library(veach_space INTERFACE)
add_library(veach::space ALIAS veach_space)
target_link_libraries(veach_space INTERFACE veach::veach)

add_subdirectory(gen)  # spike SP-05 (VCH-081): generation stages S1 and B1
target_link_libraries(veach_space INTERFACE veach_space_gen)
