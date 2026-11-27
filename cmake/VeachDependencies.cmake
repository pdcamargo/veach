# Third-party dependencies, pinned by version and SHA256.
# Policy: docs/adr/0002-language-and-build-system.md and docs/adr/0017-dependency-set.md.
# When bumping a version, update the hash, THIRD_PARTY_NOTICES.md and the ADR table.

if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    # Share downloads between build directories (presets) of this checkout.
    set(CPM_SOURCE_CACHE "${PROJECT_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM download cache")
endif()
include(CPM)

# Flecs: ECS (docs/adr/0005-ecs-flecs-v4.md). Never write v3 API code.
CPMAddPackage(
    NAME flecs
    VERSION 4.2.0
    URL https://github.com/SanderMertens/flecs/archive/refs/tags/v4.2.0.tar.gz
    URL_HASH SHA256=4107b0a36aa25f80ddcd6f939451ccfe1bacafcccdd2695840a4a8c413d05b40
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "FLECS_STATIC ON" "FLECS_SHARED OFF" "FLECS_TESTS OFF")

# Luau: scripting VM, compiler and analyzer (docs/adr/0007-scripting-luau.md).
CPMAddPackage(
    NAME luau
    VERSION 0.748
    URL https://github.com/luau-lang/luau/archive/refs/tags/0.748.tar.gz
    URL_HASH SHA256=810b13d1480a7a2efa66fe356d7e551d2ba3548823a69a4df5a6597f2beb35e5
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "LUAU_BUILD_CLI ON" "LUAU_BUILD_TESTS OFF" "LUAU_BUILD_WEB OFF")

# SDL3: windowing and input, only inside engine/platform (docs/adr/0012-platform-sdl3.md).
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

# glm: math, exposed as veach::math (docs/adr/0013-conventions-and-math.md).
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

# Tracy: profiler client, only when VEACH_TRACY=ON (docs/adr/0014-profiling-tracy.md).
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

# Slang: shader compiler, prebuilt (docs/adr/0009-shader-compilation.md). Build-time only (slangc).
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
    # D3D12 Agility SDK (docs/adr/0033-dx12-on-warp-in-ci.md): headers plus D3D12Core.dll and the debug layer,
    # deployed next to each executable by veach_deploy_d3d12(). A .nupkg is a zip; DOWNLOAD_NAME makes CMake
    # extract it.
    CPMAddPackage(
        NAME d3d12_agility
        VERSION 1.620.2
        URL https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.620.2/microsoft.direct3d.d3d12.1.620.2.nupkg
        URL_HASH SHA256=70489669c2bbeaf318b62d72b0ac3e753d521387bfc848c9763791c4e920bfcc
        DOWNLOAD_NAME microsoft.direct3d.d3d12.1.620.2.zip
        DOWNLOAD_ONLY YES)
    # WARP, the software D3D12 device (DXR 1.1, SM 6.6+). Hosted CI has no GPU, so this is what runs DX12 there.
    CPMAddPackage(
        NAME d3d12_warp
        VERSION 1.0.22
        URL https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/1.0.22/microsoft.direct3d.warp.1.0.22.nupkg
        URL_HASH SHA256=afc058f4d1066edc4231a38f3ab3592b1930f7287d4ac700e6a056e0874bb0be
        DOWNLOAD_NAME microsoft.direct3d.warp.1.0.22.zip
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

# bc7enc (richgel999/bc7enc_rdo, MIT): BC7 encoding when cooking textures, and its decoder for tests
# (docs/adr/0011-gltf-import-and-cooking.md). Only bc7enc.cpp and bc7decomp.cpp are built.
CPMAddPackage(
    NAME bc7enc
    VERSION 0.0.0-b943862
    URL https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
    URL_HASH SHA256=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6
    DOWNLOAD_ONLY YES)
add_library(veach_bc7enc STATIC "${bc7enc_SOURCE_DIR}/bc7enc.cpp" "${bc7enc_SOURCE_DIR}/bc7decomp.cpp")
target_include_directories(veach_bc7enc SYSTEM PUBLIC "${bc7enc_SOURCE_DIR}")
set_target_properties(veach_bc7enc PROPERTIES FOLDER "third_party")

# fastgltf (MIT): glTF 2.0 import (ADR 0011). Its CMake fetches simdjson 5.1.0 and simdutf 9.3.3 and checks
# their SHA256. find_package is disabled for both, so a copy installed on the machine (Homebrew) is never used
# instead of the pinned one.
set(CMAKE_DISABLE_FIND_PACKAGE_simdjson ON)
set(CMAKE_DISABLE_FIND_PACKAGE_simdutf ON)
CPMAddPackage(
    NAME fastgltf
    VERSION 1.0.1
    URL https://github.com/spnda/fastgltf/archive/refs/tags/v1.0.1.tar.gz
    URL_HASH SHA256=8399c5ed2cd90d0aef0704e65c02be59f866a9baf56bdc310cb647a6744ead94
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "FASTGLTF_DOWNLOAD_SIMDJSON ON" "FASTGLTF_DOWNLOAD_SIMDUTF ON" "FASTGLTF_ENABLE_TESTS OFF" "FASTGLTF_ENABLE_EXAMPLES OFF"
            "FASTGLTF_ENABLE_DOCS OFF")

# meshoptimizer (MIT): meshlets at cook time for the real-time renderer (ADR 0040).
CPMAddPackage(
    NAME meshoptimizer
    VERSION 1.3
    URL https://github.com/zeux/meshoptimizer/archive/refs/tags/v1.3.tar.gz
    URL_HASH SHA256=ca4f47d451201593904b9e3067fa54b65db94bc34ab5173864950862b748a145
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "MESHOPT_BUILD_SHARED_LIBS OFF" "MESHOPT_INSTALL OFF")

# LLVM libc's header-only math (libc/shared/math): backs veach::dmath, correctly rounded math for canonical code
# (docs/adr/0043-determinism-tiers-and-strict-floating-point.md, docs/research/spikes/sp-02-dmath.md). Pinned to a
# commit on main: the 23.x releases have no correctly rounded double pow (23.1.6's is wrong for large |y|) and no
# double sinh/cosh/tanh. CPM would unpack the whole monorepo (1.7 GB), so the pinned archive is checked against its
# SHA256 and only the headers are kept, in CPM's cache, by veach_fetch_archive_subset below.
set(VEACH_LLVM_LIBC_VERSION 0.0.0-e6091a8)
set(VEACH_LLVM_LIBC_COMMIT e6091a8158c863643d4fbe44b70fe62dd0ef3eba)

# veach_fetch_archive_subset(<name> <version> <url> <sha256> <top-dir> <out-var> <paths>...): downloads <url>,
# checks its SHA256, extracts <top-dir>/<path> for each path into the dependency cache, deletes the archive and sets
# <out-var> to the extracted <top-dir>. Skips everything when the cache already holds that hash and those paths.
function(veach_fetch_archive_subset name version url sha256 top out_var)
    if(DEFINED CPM_SOURCE_CACHE)
        set(cache "${CPM_SOURCE_CACHE}")
    else()
        set(cache "$ENV{CPM_SOURCE_CACHE}")
    endif()
    set(dir "${cache}/${name}/${version}")
    set(stamp "${dir}/.veach-sha256")
    file(MAKE_DIRECTORY "${cache}/${name}")
    file(LOCK "${cache}/${name}/${version}.lock" GUARD FUNCTION)
    string(JOIN " " want ${sha256} ${ARGN})
    set(have "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" have)
    endif()
    if(NOT have STREQUAL want)
        message(STATUS "veach: downloading ${name} ${version} (headers only)")
        file(REMOVE_RECURSE "${dir}")
        file(DOWNLOAD "${url}" "${dir}.archive" EXPECTED_HASH SHA256=${sha256} STATUS status TLS_VERIFY ON)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${dir}.archive")
            message(FATAL_ERROR "veach: downloading ${url} failed: ${status}")
        endif()
        set(patterns "")
        foreach(path IN LISTS ARGN)
            list(APPEND patterns "${top}/${path}")
        endforeach()
        file(ARCHIVE_EXTRACT INPUT "${dir}.archive" DESTINATION "${dir}" PATTERNS ${patterns})
        file(REMOVE "${dir}.archive")
        file(WRITE "${stamp}" "${want}")
    endif()
    set(${out_var} "${dir}/${top}" PARENT_SCOPE)
endfunction()

veach_fetch_archive_subset(llvm_libc ${VEACH_LLVM_LIBC_VERSION}
    https://github.com/llvm/llvm-project/archive/${VEACH_LLVM_LIBC_COMMIT}.tar.gz
    fe900189e827dc9b52baa42f7b01c636842944aa65edabfc2803ffa0a7543261
    llvm-project-${VEACH_LLVM_LIBC_COMMIT} _llvm_project
    libc/shared libc/src/__support libc/src/string libc/hdr libc/include/llvm-libc-macros
    libc/include/llvm-libc-types libc/LICENSE.TXT)
set(VEACH_LLVM_LIBC_DIR "${_llvm_project}/libc" CACHE INTERNAL "LLVM libc headers (veach::dmath)")
