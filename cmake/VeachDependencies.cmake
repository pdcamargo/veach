# Third-party dependencies, pinned by version and SHA256.
# Policy: docs/adr/0001-language-and-build-system.md and docs/adr/0013-dependency-set.md.
# When bumping a version, update the hash, THIRD_PARTY_NOTICES.md and the ADR table.

if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    # Share downloads between build directories (presets) of this checkout.
    set(CPM_SOURCE_CACHE "${PROJECT_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM download cache")
endif()
include(CPM)

# Flecs: ECS (docs/adr/0004-ecs-flecs-v4.md). Never write v3 API code.
CPMAddPackage(
    NAME flecs
    VERSION 4.2.0
    URL https://github.com/SanderMertens/flecs/archive/refs/tags/v4.2.0.tar.gz
    URL_HASH SHA256=4107b0a36aa25f80ddcd6f939451ccfe1bacafcccdd2695840a4a8c413d05b40
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "FLECS_STATIC ON" "FLECS_SHARED OFF" "FLECS_TESTS OFF")

# Luau: scripting VM, compiler and analyzer (docs/adr/0006-scripting-luau.md).
CPMAddPackage(
    NAME luau
    VERSION 0.748
    URL https://github.com/luau-lang/luau/archive/refs/tags/0.748.tar.gz
    URL_HASH SHA256=810b13d1480a7a2efa66fe356d7e551d2ba3548823a69a4df5a6597f2beb35e5
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "LUAU_BUILD_CLI ON" "LUAU_BUILD_TESTS OFF" "LUAU_BUILD_WEB OFF")

# SDL3: windowing and input, only inside engine/platform (docs/adr/0008-platform-sdl3.md).
if(VEACH_SDL)
    CPMAddPackage(
        NAME SDL3
        VERSION 3.4.20
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF"
            "SDL_EXAMPLES OFF" "SDL_INSTALL OFF" "SDL_UNINSTALL OFF"
            "SDL_AUDIO OFF" "SDL_GPU OFF" "SDL_RENDER OFF" "SDL_CAMERA OFF")
endif()

# glm: math, exposed as veach::math (docs/adr/0010-conventions-and-math.md).
CPMAddPackage(
    NAME glm
    VERSION 1.0.3
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
    URL_HASH SHA256=6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "GLM_BUILD_LIBRARY OFF" "GLM_BUILD_TESTS OFF" "GLM_BUILD_INSTALL OFF")

# nlohmann/json: JSON DOM for project files, .meta sidecars and schema output.
CPMAddPackage(
    NAME nlohmann_json
    VERSION 3.12.0
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "JSON_BuildTests OFF" "JSON_Install OFF")

# Tracy: profiler client, only when VEACH_TRACY=ON (docs/adr/0011-profiling-tracy.md).
if(VEACH_TRACY)
    CPMAddPackage(
        NAME tracy
        VERSION 0.15.0
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.15.0.tar.gz
        URL_HASH SHA256=3d57da54aa67d7875da1e8fecd0bd5957675cef73c27dd52bffb6ab8d290a5e6
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        OPTIONS "TRACY_ENABLE ON" "TRACY_STATIC ON" "TRACY_ONLY_LOCALHOST ON" "TRACY_NO_BROADCAST ON")
endif()

# doctest: unit tests.
if(VEACH_BUILD_TESTS)
    CPMAddPackage(
        NAME doctest
        VERSION 2.5.3
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
        OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON")
endif()

# Slang: shader compiler, prebuilt (docs/adr/0026-shader-compilation.md). Build-time only (slangc).
if(APPLE)
    CPMAddPackage(
        NAME slang
        VERSION 2026.22
        URL https://github.com/shader-slang/slang/releases/download/v2026.22/slang-2026.22-macos-aarch64.tar.gz
        URL_HASH SHA256=885b5bd9a79a1ae81fdb92212f5f7c785001f870d45c39f9bb5df01339db3fea
        DOWNLOAD_ONLY YES)
elseif(WIN32)
    CPMAddPackage(
        NAME slang
        VERSION 2026.22
        URL https://github.com/shader-slang/slang/releases/download/v2026.22/slang-2026.22-windows-x86_64.tar.gz
        URL_HASH SHA256=1273f2ad5ba35f21f77053faa1ef6eb0e1b668d52ccf1b9449014f3acdec2fba
        DOWNLOAD_ONLY YES)
    # DXC: Slang's downstream compiler for DXIL (dxcompiler.dll + dxil.dll for signing).
    CPMAddPackage(
        NAME dxc
        VERSION 1.9.2609
        URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
        URL_HASH SHA256=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1
        DOWNLOAD_ONLY YES)
endif()

# stb: PNG read/write for screenshots and golden images. Single-header, public domain / MIT.
CPMAddPackage(
    NAME stb
    VERSION 0.0.0-c16853e
    URL https://github.com/nothings/stb/archive/c16853e213f8452670d8ca2cc96c76e80d5144db.tar.gz
    URL_HASH SHA256=d8c83c763e74f1d96debedeeb6f610ee970e690ff7a5faa12498d4afedf84bdb
    DOWNLOAD_ONLY YES)
add_library(veach_stb INTERFACE)
target_include_directories(veach_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# NVIDIA FLIP: perceptual image comparison for golden-image tests (tools only). BSD-3-Clause.
CPMAddPackage(
    NAME flip
    VERSION 0.0.0-d3ab02a
    URL https://github.com/NVlabs/flip/archive/d3ab02a4425eb167d8a4021b382a2bafc6e6c838.tar.gz
    URL_HASH SHA256=991abec79184de1f983f303c34d97cd64ca463c24f74c42f6285d5bfc68af42e
    DOWNLOAD_ONLY YES)
add_library(veach_flip INTERFACE)
target_include_directories(veach_flip SYSTEM INTERFACE "${flip_SOURCE_DIR}/src/cpp")
