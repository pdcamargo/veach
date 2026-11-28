# The first slice (docs/specs/first-slice/spec.md, VCH-115): a game on the engine and the space toolbox. The module is
# a library so the tests can drive it through the input path (tests/slice_tests.cpp).
add_library(veach-slice-lib STATIC src/autopilot.cpp src/slice.cpp)
target_link_libraries(veach-slice-lib PUBLIC veach::veach veach::space)
target_include_directories(veach-slice-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(veach-slice-lib PROPERTIES FOLDER "apps")
veach_apply_warnings(veach-slice-lib)

veach_add_app(veach-slice SOURCES src/main.cpp DEPS veach-slice-lib)
# The slice finds its project (veach.project.json, scripts/) from any working directory.
target_compile_definitions(veach-slice PRIVATE VEACH_SLICE_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# The scene renderer's shaders (real-time visibility buffer and shading, the photo-mode path tracer), standard
# materials and the terrain material.
veach_add_terrain_material(veach-slice-lib)  # the planets' surfaces (docs/specs/terrain-materials/spec.md)
veach_add_scene_shaders(veach-slice-lib NAMESPACE slice_shaders)
veach_shader_library(veach-slice-lib NAMESPACE slice_shaders)
