veach_add_app(veach-lab
    SOURCES
        src/lab_module.cpp
        src/main.cpp)

# The lab finds its project (veach.project.json, scripts/) from any working directory.
target_compile_definitions(veach-lab PRIVATE VEACH_LAB_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
