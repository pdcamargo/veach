add_executable(veach-cli src/main.cpp)
set_target_properties(veach-cli PROPERTIES OUTPUT_NAME veach FOLDER "tools")
target_link_libraries(veach-cli PRIVATE veach::veach)
veach_apply_warnings(veach-cli)
