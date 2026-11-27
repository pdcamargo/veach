# 0017: Dependency set

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0013), updated to the set in the tree today.

## Context

Each dependency costs compile time, binary size, license obligations and upgrade work. We add a library when a
feature that uses it is built, not for roadmap features ahead of time.

## Decision

All dependencies are pinned by version, URL and SHA256 in `cmake/VeachDependencies.cmake` ([0002](0002-language-and-build-system.md)),
and listed with their license text in `THIRD_PARTY_NOTICES.md`.

| Dependency | Version | License | Why |
|---|---|---|---|
| Flecs | 4.2.0 | MIT | ECS, reflection, JSON, REST, modules ([0005](0005-ecs-flecs-v4.md)) |
| Luau | 0.748 | MIT | Scripting VM, compiler, `luau-analyze` ([0007](0007-scripting-luau.md)) |
| SDL3 | 3.4.20 | zlib | Windows and input, private to `engine/platform` ([0012](0012-platform-sdl3.md)) |
| glm | 1.0.3 | MIT | Math, as `veach::math` ([0013](0013-conventions-and-math.md)) |
| nlohmann/json | 3.12.0 | MIT | JSON DOM for data files and schema output |
| Tracy | 0.15.0 | BSD-3-Clause | Profiling, only with `VEACH_TRACY=ON` ([0014](0014-profiling-tracy.md)) |
| doctest | 2.5.3 | MIT | Unit tests (test builds only) |
| CPM.cmake | 0.43.2 | MIT | Dependency fetching, vendored as `cmake/CPM.cmake` |
| Slang | 2026.22 | Apache-2.0 WITH LLVM-exception | `slangc` at build time only ([0009](0009-shader-compilation.md)) |
| DirectXShaderCompiler | 1.9.2609 | LLVM Release License; `dxil.dll` Microsoft terms | DXIL at build time, Windows only |
| D3D12 Agility SDK | 1.620.2 | Microsoft terms (runtime DLLs); MIT (headers) | DX12 runtime next to each executable, Windows only ([0033](0033-dx12-on-warp-in-ci.md)) |
| Direct3D WARP | 1.0.22 | Microsoft terms, testing only, not redistributable | DX12 on GPU-less CI; must not ship |
| stb (`stb_image`, `stb_image_write`) | c16853e | MIT or public domain | Image decode, PNG screenshots ([0031](0031-image-comparison-and-goldens.md)) |
| NVIDIA FLIP | d3ab02a | BSD-3-Clause | `veach imgdiff` only |
| fastgltf | 1.0.1 | MIT | glTF import ([0011](0011-gltf-import-and-cooking.md)); pulls simdjson 5.1.0 and simdutf 9.3.3 (MIT option) |
| meshoptimizer | 1.3 | MIT | Cook-time meshlets ([0011](0011-gltf-import-and-cooking.md)) |
| bc7enc_rdo | b943862 | MIT or public domain | BC7 texture cooking; its decoder in tests |

**Choices within the set:**

- **doctest** over Catch2 and GoogleTest: lighter on compile time, single header, CTest discovery via
  `doctest_discover_tests`.
- **Our own logger** over spdlog: structured JSON lines and stable error codes ([0025](0025-structured-logging-and-errors.md))
  on top of `std::format`, with no fmt dependency.
- **nlohmann/json** over simdjson, glaze and yyjson for our data files: we need a mutable DOM, error positions and
  familiarity, not parse speed. It stays out of public headers (`json.hpp` uses `json_fwd.hpp`).
- **No CLI library:** a hand-written parser in `runtime/config.cpp` gives did-you-mean errors for free.

**Not yet added**, each waiting for the feature that needs it: physics (Box3D or Jolt, a research-phase question), a
job system, audio, networking.

**Bumping a dependency:**

1. Change version, URL and SHA256 together (`shasum -a 256` of the archive).
2. Build `debug` and `asan` and run all tests.
3. Update this table, `THIRD_PARTY_NOTICES.md` and any version mentioned in docs (e.g. the vendored Flecs docs).
4. Commit as `build(deps): bump <name> to <version>`.

## Consequences

- ccache and `EXCLUDE_FROM_ALL` keep warm builds fast despite the set's size.
- Owning the logger and CLI parser means maintaining a few hundred lines, accepted for the control over error quality.
- Every engine dependency is permissive and compatible with MIT ([0023](0023-license-mit.md)). The Microsoft runtime
  packages are build and test inputs; WARP must never ship.

## Alternatives considered

Listed per choice above.
