# 0009: Shader compilation

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0026).

## Context

Shaders are Slang ([0008](0008-shaders-slang.md)) and run on Metal and DX12. A shader that only ever compiles for one
target rots. We want shader errors at build time with `file:line`, fast app startup, no Slang runtime in shipped
apps, and C++ parameter structs that provably match the shader's.

## Decision

**Offline compilation at build time, for every target.**

- Slang is a pinned prebuilt release (v2026.22; macOS arm64 and Windows x64 packages, SHA256-pinned in
  `cmake/VeachDependencies.cmake`). `slangc` runs in the build; `libslang` is not linked.
- `veach_add_shader(<target> <file.slang> ENTRIES <entry>:<stage> ... [INCLUDES <dir>...])` in
  `cmake/VeachShaders.cmake` builds, per file:
  - **macOS:** `slangc -target metal` → MSL → `xcrun metal -std=metal3.1 -mmacosx-version-min=14.0` → AIR →
    `xcrun metallib` → `.metallib`, loaded at run time with `newLibraryWithData`;
  - **Windows:** `slangc -target dxil -profile sm_6_6`, one DXIL blob per entry, with DXC from a pinned
    DirectXShaderCompiler release first on `PATH` (`slangc` loads `dxcompiler.dll` by name);
  - **every platform:** a `-reflection-json` of the parameter block (from an HLSL compile, which needs no DXC),
    turned into `<ns>::layout::<stem>` so `rhi::parameters_match` `static_assert`s check every C++ parameter
    struct's offsets against Slang's.
- `veach_shader_library(<target> NAMESPACE <ns>)`, once per target, generates `<ns>.hpp/.cpp` that embed the bytes and
  expose `const veach::rhi::ShaderLibrary& <ns>::<file stem>()`. Lookups are explicit: no global registry.
- Materials: `veach_add_material(<target> <Name> <file.slang> [EMISSIVE])` and `veach_add_scene_shaders(<target>
  NAMESPACE <ns>)` compile the scene shaders into each app's library together with its materials
  ([0035](0035-material-model.md)).
- `slangc -depfile` feeds Ninja, so editing an imported `.slang` rebuilds its users.
- Common flags (`VEACH_SHADER_FLAGS`): `-matrix-layout-column-major` (matches `veach::math`,
  [0013](0013-conventions-and-math.md)), `-warnings-as-errors all`, and `-warnings-disable 41017` (Slang cannot see
  that `veach_scene` is filled by each entry point before use). Debug builds add `-DVEACH_VALIDATE_BINDLESS=1`
  ([0027](0027-resource-binding-model.md)).
- **Known Slang issue (2026.22):** the Metal target uses `RayDesc` as a plain struct without emitting its definition.
  `cmake/patch_msl.cmake` adds it when the MSL uses `RayDesc` and does not define it; if a future Slang emits it, the
  patch stops applying by itself. Remove it once upstream fixes it.

**At run time** the RHI takes a `ShaderLibrary` (metallib bytes plus a DXIL blob per entry) and entry names. Pipelines
are created up front by render features, never lazily inside a frame.

**Later, not now:** runtime compilation through `libslang` (shader hot reload) and a pipeline cache.

## Consequences

- Shader errors fail the build on both platforms with Slang's diagnostics; a shader that can't compile to DXIL is
  caught on Windows CI, one that can't compile to Metal on macOS.
- The macOS build needs Xcode's Metal toolchain (Command Line Tools alone are not enough); CMake checks and fails with
  that hint.
- Embedding makes binaries self-contained. Shader edits relink the owning target.
- A parameter struct that drifts from its shader is a compile error, not a GPU bug.

## Alternatives considered

- **Runtime compilation only** (libslang in every app): slower startup, errors at run time.
- **MSL source compiled with `newLibraryWithSource` at run time:** works without Xcode but moves MSL errors to startup.
- **Single-target shader builds:** cheaper, and guarantees one target rots.
- **SPIR-V plus SPIRV-Cross to MSL:** an extra translation layer for no gain; Slang emits MSL directly.
