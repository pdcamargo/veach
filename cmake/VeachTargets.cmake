# Helpers that give every Veach target the same flags. Engine areas are separate
# static libraries so that layering (docs/ARCHITECTURE.md) is enforced by the linker.

function(veach_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /external:W0)
        if(VEACH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wnull-dereference
            -Wno-gnu-zero-variadic-macro-arguments)
        if(VEACH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# veach_add_engine_library(<area> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...] [PCH ...])
# Creates target veach_<area> with alias veach::<area>. Public headers live in
# engine/<area>/include/veach/<area>/, sources in engine/<area>/src/.
function(veach_add_engine_library area)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS;PCH" ${ARGN})
    set(target veach_${area})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(veach::${area} ALIAS ${target})
    target_include_directories(${target}
        PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "engine")
    veach_apply_warnings(${target})
    if(VEACH_USE_PCH AND ARG_PCH)
        target_precompile_headers(${target} PRIVATE ${ARG_PCH})
    endif()
endfunction()

# veach_add_app(<name> SOURCES ...) — an executable that links the umbrella target.
function(veach_add_app name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE veach::veach ${ARG_DEPS})
    set_target_properties(${name} PROPERTIES FOLDER "apps")
    veach_apply_warnings(${name})
endfunction()
