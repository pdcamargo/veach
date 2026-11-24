# 0001: Language and build system

Date: 2026-11-24

## Status

Accepted

## Context

Veach targets Apple Clang (macOS) and MSVC (Windows). The minimum is C++20. Some C++23 additions remove real boilerplate, `std::expected` above all, but the two toolchains ship C++23 unevenly. Agents and humans need one obvious way to configure, build and test, and fast incremental builds. Dependencies must be reproducible: the same bytes on every machine and in CI.

## Decision

**Language.** We compile in C++23 mode (`CMAKE_CXX_STANDARD 23`, extensions off; C17 for C sources). We use only features that both Apple Clang and MSVC ship today:

- library: `std::expected`, `std::to_underlying`, `std::unreachable`, `std::format`, `<ranges>` algorithms, `std::source_location`;
- language: concepts, designated initializers, `if consteval`, `[[likely]]`, defaulted `operator<=>`.

The following are off-limits until verified on both toolchains and recorded in a new ADR: C++ modules and `import std`, `std::stacktrace`, `std::generator`, deducing `this`, `std::flat_map`, and `std::print` as a logging path.

**Build.** CMake ≥ 3.28 with `CMakePresets.json`:

- configure, build and test presets `debug`, `release`, `asan` (not Windows) and `profile`;
- workflow presets `debug` and `asan` (`cmake --workflow --preset debug`);
- the Ninja generator, with binaries in `build/<preset>/`.

Options live in `cmake/VeachOptions.cmake`, global compiler setup in `cmake/VeachCompiler.cmake`, and per-target helpers (`veach_add_engine_library`, `veach_add_app`, `veach_apply_warnings`) in `cmake/VeachTargets.cmake`.

**Dependencies.** CPM.cmake v0.43.2 is vendored as `cmake/CPM.cmake`, so configuring never downloads the package manager. Every dependency in `cmake/VeachDependencies.cmake` is pinned three ways:

- an exact `VERSION`;
- a release tarball `URL` (no git clones);
- a `URL_HASH SHA256`.

They are added `SYSTEM YES` and `EXCLUDE_FROM_ALL YES`. `CPM_SOURCE_CACHE` defaults to `.cpm-cache/` in the checkout (gitignored), so presets share downloads; an environment variable overrides it, which CI uses for caching.

## Consequences

- A fresh clone builds with only CMake, Ninja and a compiler.
- Pins are tamper-evident. Bumping one means changing version, URL and hash together, plus [0013](0013-dependency-set.md) and `THIRD_PARTY_NOTICES.md`.
- Tarballs keep configure fast (about 25 s cold, near zero warm).
- Some C++23 conveniences stay unused until MSVC and Apple Clang agree. The list above is the contract to check code against.
- On Windows the presets assume a Visual Studio developer shell (`cl` on `PATH`) for Ninja.

## Alternatives considered

- **vcpkg.** Good Windows story, but manifest and baseline versioning and per-triplet builds add a second system to learn. Pinning by baseline is coarser than one hash per dependency.
- **Conan.** Needs a Python toolchain and profiles. That is heavyweight for six dependencies, and more for agents to get wrong.
- **Raw FetchContent.** What CPM wraps. We would re-implement CPM's source cache and `OPTIONS` handling.
- **Git submodules.** Pins by commit, but makes clones slower and error-prone (`--recursive`), and puts third-party trees in our history.
- **C++20 only.** Would force a hand-written `expected`. Both compilers already ship the C++23 subset we use.
