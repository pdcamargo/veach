# Engine areas, in dependency order. Each area is its own static library so the
# layering in docs/ARCHITECTURE.md is enforced by the build:
#   core <- ecs <- platform <- asset <- script
#                  platform <- rhi <- render
#   core <- procgen (stages, field graphs, the generator sandbox; never rhi or render)
#   core <- voxel (spike SP-08; ADR 0065's full chain comes later)
#   runtime depends on everything; `veach::veach` is the umbrella users link.
add_subdirectory(core)
add_subdirectory(ecs)
add_subdirectory(platform)
add_subdirectory(asset)
add_subdirectory(script)
add_subdirectory(procgen)
add_subdirectory(voxel)
add_subdirectory(rhi)
add_subdirectory(render)
add_subdirectory(runtime)

add_library(veach INTERFACE)
add_library(veach::veach ALIAS veach)
target_include_directories(veach INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(veach INTERFACE veach::runtime veach::procgen)
