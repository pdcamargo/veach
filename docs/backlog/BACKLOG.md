# Veach backlog

Conventions: [README.md](README.md). Add items with `/backlog`.

## Now

- **VCH-002** Windows bring-up — *platform* — Build and run on real Windows hardware: SDL window, HWND handle reaching the RHI, MSVC warnings clean, DX12 stub selected with `--rhi dx12`. CI already builds and tests Windows headless. [ADR](../adr/0002-target-platforms-and-hardware.md)
- **VCH-010** Render graph and Metal RHI resources — *render* — Swapchain present on Metal, bindless resource model, command recording, a render graph with passes registered by modules. First milestone: clear plus one triangle, captured by a golden-image test (VCH-013). [ADR](../adr/0009-rhi-design.md)

## Next

- **VCH-003** MCP server — *agents* — Expose the running engine to agents over MCP. Tools: `query_entities`, `get_component`/`set_component` (through reflection and the CommandBuffer), `step_frames`, `screenshot`, `gpu_timings`, `load_scene`, and a sandboxed transactional `eval` of Luau in the live world (`ScriptRuntime::execute_source`). Builds on Flecs REST and the reflection walker.
- **VCH-004** Hot reload — *asset* — File watcher in the platform layer. Reload changed assets in place behind the same `Handle<T>`. Re-execute changed scripts, replacing their systems (systems are named, so this is a swap).
- **VCH-005** `Behavior` component — *script* — `veach.behavior(Component, { on_update = function(self, dt) ... end })` sugar for per-entity scripts, run internally as an ordinary Simulation system with batched dispatch. No separate update path.
- **VCH-006** Input in Luau and data-driven bindings — *script* — `veach.input.pressed("jump")`, `down`, `released`; action bindings in `veach.project.json` (`"input": {"jump": ["space", "w"]}`), reflected and schema-validated.
- **VCH-007** Transforms and hierarchy — *ecs* — `Transform`/`GlobalTransform` components (glTF conventions), TransformPropagation system, and parent/child exposed to Luau (`veach.spawn{ parent = e }`, `e:parent()`, `e:children()`). Other relationships stay hidden from scripts.
- **VCH-011** Views and cameras — *render* — `Camera` component (reverse-Z perspective via `veach::math`), extracted into `RenderWorld` views. Gameplay never touches GPU handles.
- **VCH-012** DX12 backend — *rhi* — `ID3D12Device` creation (DXR 1.1 required), DXGI swapchain from the HWND, capability reporting. Same `rhi::Device` interface as Metal. [ADR](../adr/0009-rhi-design.md)
- **VCH-013** Golden-image tests — *tools* — Headless render to an image, compared against LFS-stored goldens in `tests/golden/` with a perceptual tolerance. Diff images written on failure.
- **VCH-009** Slang shader pipeline — *render* — Compile Slang to MSL and DXIL at build time and at runtime (for hot reload). Shader reflection feeds the bindless layout. [ADR](../adr/0003-shaders-slang.md)

## Later

- **VCH-008** Richer script components — *script* — Integer and entity-reference fields, nested tables, arrays. Let `veach types` include an app's own modules (today it emits engine types only).
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
- **VCH-039** Windows ASan in CI — *process* — MSVC `/fsanitize=address` job (MSVC has no UBSan).

## Done

- **VCH-001** Repository bootstrap — *process* — Skeleton engine: init/shutdown order, fixed phases, Flecs barrel, reflection everywhere, command buffer, Luau API v0 with generated types, asset system with `.meta` ids, SDL3 and headless platforms, null/Metal/DX12-stub RHI, extraction boundary, the lab hello world, CI, and the docs and process files. [specs](../specs/README.md)
