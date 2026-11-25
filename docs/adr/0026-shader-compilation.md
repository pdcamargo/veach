# 0026: Shader compilation

Date: 2026-11-24

## Status

Accepted

## Context

Shaders are written in Slang ([ADR 0003](0003-shaders-slang.md)) and must run on Metal now and DX12 next. Shaders
that only ever compile for one target rot: by the time DX12 arrives, half of them would not compile to DXIL. We
also want shader errors at build time with `file:line`, fast app startup, and no Slang runtime dependency in
shipped apps yet.

## Decision

**Offline compilation at build time, for every target, from day one.**

- Slang is a pinned prebuilt release (v2026.22; macOS arm64 and Windows x64 packages, SHA256-pinned in
  `cmake/VeachDependencies.cmake`). `slangc` runs in the build; `libslang` is not linked yet.
- `veach_add_shader(<target> <file.slang> ENTRIES <entry>:<stage> ...)` (in `cmake/VeachShaders.cmake`) builds,
  per shader file:
  - **macOS:** `slangc -target metal` → MSL → `xcrun metal -std=metal3.1` → AIR → `xcrun metallib` → `.metallib`.
    Compiling the MSL at build time validates it and gives the runtime a ready library (`newLibraryWithData`).
  - **Windows:** `slangc -target dxil -profile sm_6_6`, one DXIL blob per entry point, using DXC from a pinned
    DirectXShaderCompiler release (put first on `PATH` for the `slangc` call; `slangc` loads `dxcompiler.dll`
    by name). There is no DX12 backend yet. The blobs are still built, so every shader is proven portable on
    every Windows CI run.
- `veach_shader_library(<target> NAMESPACE <ns>)`, once per target, generates `<ns>.hpp/.cpp`. They embed the
  bytes and expose, for each file, `const veach::rhi::ShaderLibrary& <ns>::<file stem>()`. Code passes the library to the pipeline it
    creates. Lookups are explicit: no global registry, nothing for a static library to drop.
- `slangc -depfile` feeds Ninja, so editing an imported `.slang` file rebuilds its users.
- Common flags: `-matrix-layout-column-major` (matches `veach::math`, [ADR 0010](0010-conventions-and-math.md))
  and `-warnings-as-errors all`. Shader debug info is not emitted yet.
- **Known Slang issue (2026.22):** the Metal target uses `RayDesc` as a plain struct but does not emit its
  definition. `cmake/patch_msl.cmake` adds the definition when the MSL uses `RayDesc` and does not define it. If a
  future Slang emits it, the patch stops applying by itself. Remove the patch once upstream fixes it (VCH-043).

**At runtime** the RHI takes a `ShaderLibrary` (metallib bytes plus a DXIL blob per entry) and entry-point names.
Metal loads the library once per pipeline creation. Pipelines are created up front by render features, never
lazily inside a frame.

**Later, not now:** runtime compilation through `libslang` for shader hot reload (VCH-004), and a pipeline cache.

## Consequences

- Shader errors fail the build, on both platforms, with Slang's diagnostics.
- A shader that can't compile to DXIL is caught on Windows CI. One that can't compile to Metal is caught on macOS.
- The macOS build needs the Metal toolchain. Xcode provides it, locally and on GitHub runners; Command Line Tools
  alone are not enough. CMake checks for it and fails with that hint.
- Embedding makes binaries self-contained. Shader edits relink the owning target, which is fast at our size.
- DXIL blobs are unsigned until the DX12 backend runs them. DXC signs when `dxil.dll` is next to it, and the
  pinned release ships it.

## Alternatives considered

- **Runtime compilation only** (libslang in every app): slower startup, errors at run time, and Slang becomes a
  runtime dependency before we need hot reload.
- **MSL source embedded, compiled with `newLibraryWithSource` at run time:** works without Xcode, but moves MSL
  errors to run time and startup.
- **Metal-only shader builds until the DX12 backend exists:** cheaper today, guaranteed pain next week.
- **SPIR-V as an intermediate (SPIRV-Cross to MSL):** an extra translation layer for no gain; Slang emits MSL
  directly.
