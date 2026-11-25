#!/usr/bin/env bash
# Type-checks Windows sources on macOS (docs/guides/local-tools.md): clang-cl -fsyntax-only against the MSVC
# CRT and Windows SDK headers in ~/.xwin and the pinned D3D12 Agility SDK headers. MSVC in CI stays the judge.
#   tools/scripts/wincheck.sh engine/rhi/src/dx12/dx12_device.cpp [more files...]
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
xwin="${XWIN_DIR:-$HOME/.xwin}"
clang_cl="${CLANG_CL:-/opt/homebrew/opt/llvm/bin/clang-cl}"
[ -d "$xwin/sdk" ] || { echo "wincheck: $xwin not found (see docs/guides/local-tools.md)" >&2; exit 1; }

# The Agility SDK headers, at the version and hash pinned in cmake/VeachDependencies.cmake, cached in build/.
version=$(sed -n '/NAME d3d12_agility/,/DOWNLOAD_ONLY/s/.*VERSION \([0-9.]*\).*/\1/p' "$root/cmake/VeachDependencies.cmake")
sha=$(sed -n '/NAME d3d12_agility/,/DOWNLOAD_ONLY/s/.*SHA256=\([0-9a-f]*\).*/\1/p' "$root/cmake/VeachDependencies.cmake")
agility="$root/build/wincheck/agility-$version"
if [ ! -d "$agility/build/native/include" ]; then
    mkdir -p "$agility"
    curl -sSL -o "$agility.zip" \
        "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$version/microsoft.direct3d.d3d12.$version.nupkg"
    echo "$sha  $agility.zip" | shasum -a 256 -c - >/dev/null
    unzip -q -o "$agility.zip" -d "$agility"
fi

includes=()
for area in "$root"/engine/*/include "$root"/engine/include "$root"/engine/rhi/src "$root"/tests; do
    includes+=(/I "$area")
done
for dep in flecs/*/include glm/* doctest/* nlohmann_json/*/include; do
    for dir in "$root"/.cpm-cache/$dep; do includes+=(/imsvc "$dir"); done
done

for file in "$@"; do
    "$clang_cl" --target=x86_64-pc-windows-msvc -fms-compatibility-version=19.44 /std:c++latest /EHsc /W4 /WX \
        /permissive- -fsyntax-only -Wno-unused-command-line-argument -Wno-microsoft-include \
        -Wno-nonportable-include-path -Wno-unknown-pragmas \
        /imsvc "$xwin/crt/include" /imsvc "$xwin/sdk/include/ucrt" /imsvc "$xwin/sdk/include/um" \
        /imsvc "$xwin/sdk/include/shared" /imsvc "$xwin/sdk/include/winrt" \
        /I "$agility/build/native/include" "${includes[@]}" \
        /DNOMINMAX /DWIN32_LEAN_AND_MEAN '/DVEACH_SOURCE_DIR="."' "$file"
    echo "wincheck: $file ok"
done
