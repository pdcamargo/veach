add_executable(veach-tests
    main.cpp
    asset_tests.cpp
    core_tests.cpp
    ecs_tests.cpp
    lifecycle_tests.cpp
    platform_tests.cpp
    script_tests.cpp)
target_link_libraries(veach-tests PRIVATE veach::veach doctest::doctest)
target_compile_definitions(veach-tests PRIVATE VEACH_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(veach-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(veach-tests PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-tests)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(veach-tests TEST_PREFIX "unit: ")

# ---- Integration checks (acceptance criteria that need the real binaries) ----

set(_hello "Hello from Luau — frame 1.*Hello from Luau — frame 2.*Hello from Luau — frame 3.*shutdown core")
add_test(NAME "lab: hello world exits 0"
    COMMAND veach-lab --script scripts/hello.luau --frames 3 --headless)
add_test(NAME "lab: hello world output"
    COMMAND veach-lab --script scripts/hello.luau --frames 3 --headless)
set_tests_properties("lab: hello world output" PROPERTIES
    PASS_REGULAR_EXPRESSION "${_hello}"
    FAIL_REGULAR_EXPRESSION "\\] error ")
add_test(NAME "lab: json logs" COMMAND veach-lab --frames 1 --headless --log-format json)
set_tests_properties("lab: json logs" PROPERTIES PASS_REGULAR_EXPRESSION "\\{\"t\":[0-9.]+,\"level\":\"info\",\"cat\":\"script\",\"msg\":\"Hello from Luau")
add_test(NAME "lab: --help" COMMAND veach-lab --help)
set_tests_properties("lab: --help" PROPERTIES PASS_REGULAR_EXPRESSION "--frames N")

file(GLOB_RECURSE _luau_scripts RELATIVE "${PROJECT_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/apps/*.luau" "${PROJECT_SOURCE_DIR}/tests/scripts/*.luau")
add_test(NAME "luau-analyze: scripts type-check under --!strict"
    COMMAND Luau.Analyze.CLI ${_luau_scripts} types/veach.d.luau
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

add_test(NAME "generated: types/veach.d.luau is up to date"
    COMMAND veach-cli types --check types/veach.d.luau
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME "generated: schemas/ are up to date"
    COMMAND veach-cli schemas --check schemas
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME "repo: hygiene (backlog ids, veach names in apps, CLAUDE.md)"
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/scripts/check_repo.py"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
endif()

# Leak checks (macOS `leaks`, ADR 0012). Not under ASan: leaks cannot inspect sanitized processes.
if(VEACH_LEAK_CHECK)
    if(NOT APPLE OR VEACH_SANITIZE)
        message(FATAL_ERROR "VEACH_LEAK_CHECK needs macOS and no VEACH_SANITIZE (use the 'leaks' preset)")
    endif()
    set(_leak_check sh "${PROJECT_SOURCE_DIR}/tools/scripts/leak-check.sh")
    add_test(NAME "leaks: unit tests" COMMAND ${_leak_check} $<TARGET_FILE:veach-tests>)
    add_test(NAME "leaks: lab headless run"
        COMMAND ${_leak_check} $<TARGET_FILE:veach-lab> --script scripts/hello.luau --frames 3 --headless)
endif()

# Benchmarks: built with the tests, run by hand (not part of ctest).
add_executable(veach-bench-script bench/script_bench.cpp)
target_link_libraries(veach-bench-script PRIVATE veach::veach)
set_target_properties(veach-bench-script PROPERTIES FOLDER "tests")
veach_apply_warnings(veach-bench-script)
