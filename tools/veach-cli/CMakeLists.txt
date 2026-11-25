add_executable(veach-cli src/main.cpp src/imgdiff.cpp)
set_target_properties(veach-cli PROPERTIES OUTPUT_NAME veach FOLDER "tools")
target_link_libraries(veach-cli PRIVATE veach::veach veach_flip)
veach_apply_warnings(veach-cli)
