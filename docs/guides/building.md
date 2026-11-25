# Building and testing

Decisions behind this: [ADR 0001](../adr/0001-language-and-build-system.md) (C++23 subset, CMake presets, CPM) and [ADR 0012](../adr/0012-code-quality-tooling.md) (warnings, sanitizers, ccache, PCH, formatting).

## Prerequisites

| | macOS (Apple Silicon) | Windows |
|---|---|---|
| Compiler | Apple Clang from **Xcode** (the Metal shader compiler ships with Xcode, not with the Command Line Tools) | MSVC from Visual Studio 2022; build from a **Developer PowerShell / x64 Native Tools prompt** so `cl` and Ninja are on `PATH` |
| CMake | ≥ 3.29 | ≥ 3.29 |
| Generator | Ninja | Ninja |
| Git | with **Git LFS** (`git lfs install`): golden images live in LFS | same |
| GPU | Metal 3 with ray tracing (Apple M1 or newer) to run the GPU golden tests; they skip otherwise | None needed: without a D3D12 GPU that has SM 6.6, enhanced barriers and DXR 1.1, DX12 runs on WARP, which the build deploys ([ADR 0031](../adr/0031-dx12-on-warp-in-ci.md)). `VEACH_DX12_ADAPTER=warp` forces it |
| Optional | `ccache` (used automatically when found), Python 3 (repo hygiene test) | same |

Dependencies are downloaded by CMake on first configure (see [CPM cache](#dependency-cache)). That includes the Slang
shader compiler (prebuilt, macOS and Windows) and, on Windows, DXC. Nothing else needs installing.

## Presets

```sh
cmake --workflow --preset debug        # configure + build + test, the normal loop
cmake --preset debug && cmake --build --preset debug && ctest --preset debug   # same, step by step
```

| Preset | What it is |
|---|---|
| `debug` | Debug build. Use this unless you need something else. |
| `release` | Optimized build. |
| `asan` | Debug + AddressSanitizer + UndefinedBehaviorSanitizer. Clang only (not available on Windows). `cmake --workflow --preset asan` |
| `leaks` | Debug + macOS `leaks` checks: `ctest --preset leaks` runs the unit tests and a headless lab run under `leaks --atExit` and fails on any leak. macOS only; cannot be combined with ASan. |
| `profile` | RelWithDebInfo + Tracy (`VEACH_TRACY=ON`). See [profiling.md](profiling.md). |

Each preset builds into `build/<preset>/`. Binaries:

- `build/<preset>/apps/lab/veach-lab` — the lab app
- `build/<preset>/tools/veach-cli/veach` — the `veach` CLI (`types`, `schemas`)
- `build/<preset>/tests/veach-tests` — unit tests
- `build/<preset>/_deps/luau-build/luau-analyze` — the Luau type checker, built from the pinned Luau

## Build options

Defined in `cmake/VeachOptions.cmake`; pass with `-D` or edit `CMakeUserPresets.json`.

| Option | Default | Effect |
|---|---|---|
| `VEACH_BUILD_TESTS` | `ON` | Build `veach-tests` and register the CTest suite. |
| `VEACH_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX` on Veach targets (not on dependencies). |
| `VEACH_TRACY` | `OFF` | Fetch the Tracy client and turn `VEACH_ZONE` macros into real zones. |
| `VEACH_USE_CCACHE` | `ON` | Use `ccache` as compiler launcher if installed. |
| `VEACH_USE_PCH` | `ON` | Precompiled headers for Flecs-, Luau- and JSON-heavy engine targets. |
| `VEACH_SDL` | `ON` | Build the SDL3 platform backend. The headless backend is always built. |
| `VEACH_LEAK_CHECK` | `OFF` | Registers the `leaks:` CTest tests (`tools/scripts/leak-check.sh`). The `leaks` preset sets it. |
| `VEACH_SANITIZE` | empty | Sanitizer list, e.g. `address;undefined` (the `asan` preset sets it). |

## Dependency cache

CPM downloads pinned, hash-checked tarballs (`cmake/VeachDependencies.cmake`) into
`.cpm-cache/` at the repo root, shared by all presets of a checkout. Set the
`CPM_SOURCE_CACHE` environment variable to share one cache across checkouts
(CI does this). `.cpm-cache/` and `build/` are git-ignored.

## Running

```sh
build/debug/apps/lab/veach-lab --script scripts/hello.luau --frames 3 --headless
build/debug/apps/lab/veach-lab --window
build/debug/apps/lab/veach-lab --help        # every core flag
```

A run exits with code 0 only if nothing was logged at error level.

## Tests

`ctest --preset debug` runs everything:

- `unit: ...` — doctest cases from `tests/*.cpp`. Cases that verify a spec acceptance criterion are named `[area.ACn] ...`.
- `lab: ...` — the hello world as a real process (exit code, output, JSON logs, `--help`).
- `luau-analyze: ...` — every `.luau` under `apps/` type-checks under `--!strict` against `types/veach.d.luau`.
- `generated: ...` — `types/veach.d.luau` and `schemas/` match what the code generates.
- `repo: hygiene ...` — `tools/scripts/check_repo.py` (TODOs reference backlog ids, no `flecs::` in `apps/`, `CLAUDE.md` is one line). Skipped if Python 3 is missing.

Run a subset:

```sh
ctest --preset debug -R "unit:"                      # only unit tests
build/debug/tests/veach-tests -tc="*script*"          # doctest filter by test-case name
build/debug/tests/veach-tests -tc="*lifecycle.AC1*" -s   # one criterion, show passing asserts too
```

## Regenerating generated files

Both are generated from Flecs reflection and checked by CTest. Regenerate after
changing a reflected type, a `Scriptable` component or the Luau API, and commit the result:

```sh
build/debug/tools/veach-cli/veach types --out types/veach.d.luau
build/debug/tools/veach-cli/veach schemas --out schemas
```

`--check <path>` instead of `--out` exits 1 if the file on disk is stale (what CTest runs).

## Golden images

`ctest` runs `golden: ...` tests on macOS: the lab renders with `--screenshot`, and `veach imgdiff` (NVIDIA FLIP)
compares the result with `tests/golden/*.png`. They report **SKIPPED** on GPUs without Metal 3 and ray tracing
(for example GitHub's hosted runners). Set `VEACH_REQUIRE_GPU=1` to make that a failure. To regenerate goldens
on purpose:

```sh
cmake --build --preset debug --target update-goldens   # then look at tests/golden/*.png before committing
```

Details: [rendering guide](rendering.md).

## Formatting

`.clang-format` at the repo root is the style. Use clang-format **23** (LLVM 23,
e.g. `brew install llvm`, or `pip install clang-format==23.1.5`, the version CI pins):

```sh
clang-format -i engine/core/src/log.cpp          # one file
git ls-files '*.cpp' '*.hpp' '*.mm' | xargs clang-format --dry-run --Werror   # what CI checks
```
