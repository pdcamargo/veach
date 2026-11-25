# Veach backlog

Conventions: [README.md](README.md). Add items with `/backlog`.

## Now

- **VCH-002** Windows bring-up — *platform* — Build and run on real Windows hardware: SDL window, HWND handle reaching the RHI, MSVC warnings clean, DX12 stub selected with `--rhi dx12`. CI already builds and tests Windows headless. [ADR](../adr/0002-target-platforms-and-hardware.md)
- **VCH-044** GPU runner for CI — *process* — Self-hosted Mac runner (decided 2026-11-25): separate non-admin user, no secrets, push to main and manual dispatch only, `VEACH_REQUIRE_GPU=1`. Script and guide in [ci-runners](../guides/ci-runners.md). Done once the `macOS GPU` job passes on `main`.

## Next

- **VCH-003** MCP server — *agents* — Expose the running engine to agents over MCP. Tools: `query_entities`, `get_component`/`set_component` (through reflection and the CommandBuffer), `step_frames`, `screenshot`, `gpu_timings`, `load_scene`, and a sandboxed transactional `eval` of Luau in the live world (`ScriptRuntime::execute_source`). Builds on Flecs REST and the reflection walker.
- **VCH-004** Hot reload — *asset* — File watcher in the platform layer. Reload changed assets in place behind the same `Handle<T>`. Re-execute changed scripts, replacing their systems (systems are named, so this is a swap).
- **VCH-005** `Behavior` component — *script* — `veach.behavior(Component, { on_update = function(self, dt) ... end })` sugar for per-entity scripts, run internally as an ordinary Simulation system with batched dispatch. No separate update path.
- **VCH-006** Input in Luau and data-driven bindings — *script* — `veach.input.pressed("jump")`, `down`, `released`; action bindings in `veach.project.json` (`"input": {"jump": ["space", "w"]}`), reflected and schema-validated.
- **VCH-007** Transforms and hierarchy — *ecs* — `Transform`/`GlobalTransform` components (glTF conventions), TransformPropagation system, and parent/child exposed to Luau (`veach.spawn{ parent = e }`, `e:parent()`, `e:children()`). Other relationships stay hidden from scripts.
- **VCH-011** Views and cameras — *render* — `Camera` component (reverse-Z perspective via `veach::math`), extracted into `RenderWorld` views. Gameplay never touches GPU handles.
- **VCH-012** DX12 backend — *rhi* — Implement the stub `Dx12Device` (it already compiles against the full interface): ID3D12Device with DXR 1.1 and SM 6.6, enhanced barriers mapped from `ResourceState`, a shader-visible CBV/SRV/UAV heap plus a sampler heap for bindless descriptors, root constants for parameter blocks, a DXGI swapchain from the HWND, and readback. Shaders already compile to DXIL. [ADR 0024](../adr/0024-resource-binding-model.md), [ADR 0025](../adr/0025-sync-and-barrier-model.md)
- **VCH-009** Runtime shader compilation — *render* — Build-time Slang → metallib/DXIL exists ([ADR 0026](../adr/0026-shader-compilation.md)). Add libslang at run time for shader hot reload, and a pipeline cache.

## Later

- **VCH-008** Richer script components — *script* — Nested tables and arrays in script components (explicit scalar, entity and optional types exist). Let `veach types` include an app's own modules (today it emits engine types only).
- **VCH-014** Record/replay — *runtime* — Record seed, input and script command logs; replay a run bit-for-bit. Depends on the determinism rules (fixed step, seeded RNG, no wall clock in simulation).
- **VCH-015** Job system — *core* — enkiTS or Taskflow (decide by ADR), with Flecs worker threads hooked into it. Everything is single-threaded until this lands.
- **VCH-016** Tessera: virtualized geometry — *render* — Cluster hierarchies, visibility buffer, GPU culling, streaming. Our Nanite.
- **VCH-017** Candela: real-time global illumination — *render* — Hardware-raytraced, denoised GI within real-time budgets. Our Lumen.
- **VCH-018** Raytraced reflections — *render* — With screen-space reflections as hybrid and fallback.
- **VCH-019** Physical sky and atmosphere — *render* — Sky, aerial perspective, sun and sky lighting feeding Candela.
- **VCH-020** Volumetric fog — *render* — Froxel-based, lit by the same light structures.
- **VCH-021** Soft shadows from area lights — *render* — Raytraced, with many-light sampling.
- **VCH-022** Denoiser — *render* — Shared denoising for GI, reflections and shadows.
- **VCH-023** Upscaler — *render* — Temporal upscaling to hit 60 fps at 1440p output; evaluate MetalFX and DLSS/FSR integration behind one interface.
- **VCH-024** Extendable materials — *render* — Materials as Slang modules implementing interfaces with default implementations, so users override only `evalAlbedo`, `evalEmission`, `displaceVertex`, and so on (like Bevy's `ExtendedMaterial`).
- **VCH-025** glTF import — *asset* — fastgltf loader, meshoptimizer processing, meshes and materials as assets with `.meta` import settings.
- **VCH-026** Texture pipeline — *asset* — KTX2 with BC7 (and ASTC where useful), mip generation, import settings in `.meta`.
- **VCH-027** Scenes and prefabs — *asset* — Flecs Script scenes/prefabs as assets; `load_scene`. [ADR](../adr/0007-asset-formats-and-identity.md)
- **VCH-028** Physics with Box3D — *physics* — Integrate Erin Catto's Box3D (MIT, C17, alpha, cross-platform deterministic). Steps in Simulation at the fixed 60 Hz.
- **VCH-029** Audio — *audio* — Pick a library by ADR; spatial audio fed from transforms.
- **VCH-030** UI — *ui* — In-game UI. Approach to be decided by ADR.
- **VCH-031** Async asset loading — *asset* — Loaders on worker threads (VCH-015), `wait()` blocking, handle ref-counting and unloading.
- **VCH-032** Asset id index — *asset* — Scan `.meta` files at startup to resolve ids to paths; `load_by_id`; detect moved and renamed files.
- **VCH-033** Editor — *editor* — Built on the same reflection, MCP and command-buffer machinery agents use.
- **VCH-034** Luau native codegen and script budgets — *script* — Enable Luau.CodeGen where supported; interrupt callbacks for runaway scripts, with per-invocation time budgets.
- **VCH-035** Script dispatch allocations — *script* — Batch building is ~92 ns/entity (9.2 ms at 100k, M5 Max), almost all of it allocating an Entity userdata and a row table per component. Reuse them across runs (row values become views valid during the call). Measure with `veach-bench-script` before and after. [spec](../specs/scripting-luau-api-v0/spec.md)
- **VCH-036** Gamepads — *platform* — SDL gamepad events into `Input` and actions.
- **VCH-037** Window polish — *platform* — Exclusive fullscreen, pixel-size and DPI fields, multi-monitor placement.
- **VCH-038** Blocking clang-tidy in CI — *process* — Make the `.clang-tidy` baseline clean, then run `run-clang-tidy` as a required CI job.
- **VCH-043** Drop the MSL `RayDesc` patch — *render* — Remove `cmake/patch_msl.cmake` once Slang's Metal target emits `RayDesc` (reported against 2026.22). [ADR](../adr/0026-shader-compilation.md)
- **VCH-047** Windows GPU runner script — *process* — Turn the manual Windows steps in [ci-runners](../guides/ci-runners.md) into a PowerShell setup/teardown pair, once the DX12 goldens exist.
- **VCH-039** Windows ASan in CI — *process* — MSVC `/fsanitize=address` job (MSVC has no UBSan).

## Done

- **VCH-041** Parameter-block layout checks — *render* — Each shader's parameter block is reflected (Slang, HLSL layout) into `<ns>::layout::<stem>`. `static_assert(rhi::parameters_match<T>(...))` compares names, order, offsets and sizes at compile time. The shader build rejects `float3`/3-row matrices. The hygiene check requires a layout check for every shader with parameters. [ADR 0024](../adr/0024-resource-binding-model.md)

- **VCH-045** Frames in flight — *rhi* — N frames in flight (default 2) on a shared `SubmissionTracker`: monotonic submission ids, per-slot fences, per-frame TLAS instance buffers, and fence-tracked retirement for `destroy()` (absorbs VCH-042). Pooled transients sync with their previous frame (`Barrier::discard`). [ADR 0029](../adr/0029-frame-and-sync-model.md)

- **VCH-010** First light: RHI v0 and render graph v0 — *render* — Metal backend with buffers, textures, samplers, graphics and compute pipelines, BLAS/TLAS build and refit, swapchain, offscreen targets, readback, Metal validation in debug, and GPU errors logged at error level. Bindless through `DescriptorHandle`. The render graph derives order, culling and barriers. A rasterized triangle and a ray-query triangle. [spec](../specs/first-light/spec.md)
- **VCH-013** Golden-image tests — *tools* — `--screenshot`, `veach imgdiff` (NVIDIA FLIP), goldens in Git LFS, an `update-goldens` target, and a skip on unsupported GPUs (`VEACH_REQUIRE_GPU=1` makes it a failure). [spec](../specs/first-light/spec.md)

- **VCH-001** Repository bootstrap — *process* — Skeleton engine: init/shutdown order, fixed phases, Flecs barrel, reflection everywhere, command buffer, Luau API v0 with generated types, asset system with `.meta` ids, SDL3 and headless platforms, null/Metal/DX12-stub RHI, extraction boundary, the lab hello world, CI, and the docs and process files. [specs](../specs/README.md)
