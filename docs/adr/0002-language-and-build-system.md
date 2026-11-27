# 0002: Language and build system

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0001).

## Context

Veach builds with Apple Clang (macOS) and MSVC (Windows), which ship C++23 unevenly. Agents and humans need one
obvious way to configure, build and test, with fast incremental builds. Dependencies must be reproducible: the same
bytes on every machine and in CI.

## Decision

**Language.** C++23 mode (`CMAKE_CXX_STANDARD 23`, extensions off; C17 for C sources), using only what both Apple
Clang and MSVC ship today:

- library: `std::expected`, `std::to_underlying`, `std::unreachable`, `std::format`, `<ranges>` algorithms,
  `std::source_location`;
- language: concepts, designated initializers, `if consteval`, `[[likely]]`, defaulted `operator<=>`.

Off-limits until verified on both toolchains and recorded in a new ADR: C++ modules and `import std`,
`std::stacktrace`, `std::generator`, deducing `this`, `std::flat_map`, and `std::print` as a logging path.

**Build.** CMake ≥ 3.28 with `CMakePresets.json`:

- configure, build and test presets `debug`, `release`, `asan` (not Windows), `leaks` (macOS) and `profile`;
- workflow presets `debug` and `asan` (`cmake --workflow --preset debug`);
- the Ninja generator, binaries in `build/<preset>/`.

Options live in `cmake/VeachOptions.cmake`, compiler setup in `cmake/VeachCompiler.cmake`, per-target helpers
(`veach_add_engine_library`, `veach_add_app`, `veach_apply_warnings`) in `cmake/VeachTargets.cmake`, shader helpers
in `cmake/VeachShaders.cmake`.

**Dependencies.** CPM.cmake v0.43.2 is vendored as `cmake/CPM.cmake`, so configuring never downloads the package
manager. Every dependency in `cmake/VeachDependencies.cmake` is pinned three ways: an exact `VERSION`, a release
tarball (or NuGet package) `URL` with no git clones, and a `URL_HASH SHA256`. They are added `SYSTEM YES` and
`EXCLUDE_FROM_ALL YES`. `CPM_SOURCE_CACHE` defaults to `.cpm-cache/` in the checkout (gitignored); an environment
variable overrides it, which CI uses for caching.

## Consequences

- A fresh clone builds with CMake, Ninja and a compiler (plus Xcode's Metal toolchain on macOS, [0009](0009-shader-compilation.md)).
- Pins are tamper-evident. Bumping one means changing version, URL and hash together, plus
  [0017](0017-dependency-set.md) and `THIRD_PARTY_NOTICES.md`.
- Some C++23 conveniences stay unused until MSVC and Apple Clang agree. The list above is the contract.
- On Windows the presets assume a Visual Studio developer shell (`cl` on `PATH`) for Ninja.

## Alternatives considered

- **vcpkg.** Good on Windows, but baseline versioning and per-triplet builds are a second system to learn, and
  pinning by baseline is coarser than one hash per dependency.
- **Conan.** Needs a Python toolchain and profiles; more for agents to get wrong.
- **Raw FetchContent.** What CPM wraps; we would re-implement its source cache and `OPTIONS` handling.
- **Git submodules.** Slower, error-prone clones (`--recursive`) and third-party trees in our history.
- **C++20 only.** Would force a hand-written `expected`.
