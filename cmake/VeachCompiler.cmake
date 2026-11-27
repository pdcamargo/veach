# Language standard, compiler launcher and sanitizers. Applies to the whole build
# (including dependencies) where it must be global; per-target warning flags live
# in VeachTargets.cmake. See docs/adr/0002-language-and-build-system.md.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)

# ccache cannot cache MSVC /Zi (separate PDB) builds; embed debug info instead.
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

if(VEACH_USE_CCACHE)
    find_program(VEACH_CCACHE_PROGRAM ccache)
    if(VEACH_CCACHE_PROGRAM)
        # Precompiled headers need relaxed ccache checks to stay cacheable.
        set(_launcher "${CMAKE_COMMAND};-E;env;CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime;${VEACH_CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${_launcher}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_launcher}")
        set(CMAKE_OBJCXX_COMPILER_LAUNCHER "${_launcher}")
        message(STATUS "veach: using ccache (${VEACH_CCACHE_PROGRAM})")
    endif()
endif()

if(VEACH_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "VEACH_SANITIZE is only supported with Clang/GCC. Use the 'debug' preset on Windows.")
    endif()
    list(JOIN VEACH_SANITIZE "," _sanitizers)
    add_compile_options(-fsanitize=${_sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${_sanitizers})
    message(STATUS "veach: sanitizers enabled (${_sanitizers})")
endif()

if(MSVC)
    add_compile_options(/utf-8 /Zc:preprocessor /Zc:__cplusplus)
endif()

# Canonical floating point (docs/adr/0043-determinism-tiers-and-strict-floating-point.md): every CPU target, and every
# dependency built after this point, rounds `a * b + c` as two operations, never a fused multiply-add, so the same
# source gives the same bits on Apple Clang (which contracts by default on arm64) and MSVC. No fast math anywhere.
# tools/scripts/check_repo.py fails if these go missing; the canonical-hash test fails if they stop working.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off -fno-fast-math)
endif()
