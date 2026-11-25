# Veach backlog

Conventions: [README.md](README.md). Add items with `/backlog`.

## Now

## Next

- **VCH-055** Sponza golden — *render* — Add Khronos's Sponza next to FlightHelmet. Waiting on the owner: its files are under the CRYENGINE Limited License Agreement, which the agent may not accept on their behalf (AGENTS.md). [spec](../specs/first-real-scene/spec.md)
- **VCH-005** `Behavior` component — *script* — `veach.behavior(Component, { on_update = function(self, dt) ... end })` sugar for per-entity scripts, run internally as an ordinary Simulation system with batched dispatch. No separate update path.
- **VCH-009** Runtime shader compilation — *render* — Build-time Slang → metallib/DXIL exists ([ADR 0026](../adr/0026-shader-compilation.md)). Add libslang at run time for shader hot reload, and a pipeline cache.

## Later

- **VCH-048** MCP over HTTP — *agents* — Streamable HTTP transport next to stdio, so several clients can attach to one running app (an editor session). Same tools. [spec](../specs/mcp-server/spec.md)
- **VCH-049** Per-pass GPU timings — *render* — Timestamp queries per render-graph pass (Metal counter sample buffers, DX12 timestamp queries); `gpu_timings` returns them. Today it reports the whole frame.
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
- **VCH-027** Scenes and prefabs — *asset* — Flecs Script scenes/prefabs as assets; `load_scene`, also as an MCP tool. [ADR](../adr/0007-asset-formats-and-identity.md)
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
- **VCH-039** Windows ASan in CI — *process* — MSVC `/fsanitize=address` job (MSVC has no UBSan).

## Blocked

- **VCH-050** DX12 on real hardware — *rhi* — Run the DX12 goldens and a long frames-in-flight soak on a physical DXR 1.1 GPU (vendor drivers, real timings), and present to a real display: on the hosted runner about one `Present` in 60 fails with `E_INVALIDARG`, with or without a resize, and no debug layer explains it; the windowed test tolerates exactly that, only under `VEACH_CI_HOSTED_WINDOWS=1`. Blocked: no Windows GPU machine or runner ([ADR 0030](../adr/0030-windows-verified-in-hosted-ci.md)). Unblocked by a paid GPU runner or a Windows machine registered as a runner.

## Done

- **VCH-044** GPU runner for CI — *process* — Won't do (2026-11-25): no self-hosted runner; the pre-push gate runs the Metal goldens on a real Mac. [ADR](../adr/0036-no-self-hosted-gpu-runner.md)
- **VCH-054** Reference path tracer — *render* — Progressive, unbiased: GGX + Lambert, NEE to punctual lights and emissive triangles with MIS, alpha by continuing opaque rays (ADR 0035), PCG per pixel and sample; restarts on any change. MCP `path_trace`. Cornell box and FlightHelmet goldens on Metal and DX12/WARP. [ADR](../adr/0034-lights-and-reference-path-tracer.md)
- **VCH-053** Lights — *render* — `DirectionalLight` (lux), `PointLight`/`SpotLight` (cd) from components or glTF `KHR_lights_punctual`, emissive surfaces (nits), `Camera.ev100`. [ADR](../adr/0034-lights-and-reference-path-tracer.md)
- **VCH-052** Materials v0 — *render* — `IMaterial` with overridable defaults (Slang `override`), generated `evaluateSurface`/`evaluateAlpha` dispatch, `veach_add_material()`; the lab's `GlowMaterial` overrides only `evalEmission`. [ADR](../adr/0033-material-model.md)
- **VCH-051** glTF import and cooking — *asset* — fastgltf into MeshAsset/TextureAsset (KTX2 BC7)/MaterialAsset/SceneAsset, derived sub-asset ids, a cook cache, `SceneInstance`. [ADR](../adr/0032-asset-import-and-cooking.md)
- **VCH-004** Hot reload — *asset* — Assets reload in place behind the same handle (`fs::stamp` polling); a reloaded script runs as if fresh (systems swapped by name, top-level entities respawned), transactionally. Windowed runs poll every 250 ms; headless only on request. MCP `reload` and `list_assets`. Shader reload stays VCH-009. [spec](../specs/hot-reload/spec.md)
- **VCH-006** Input actions in Luau — *script* — `"input"` bindings in `veach.project.json` (keys and mouse buttons, schema-validated), `veach.input.down/pressed/released/axis`, platform-level injection and the MCP tool `send_input`. The lab's fly camera is a script, tested end to end over MCP. Mouse motion and gamepads are later. [spec](../specs/input/spec.md)
- **VCH-007** Transforms and hierarchy — *ecs* — `GlobalTransform` (added with every `Transform`), cascade propagation over ChildOf per fixed step and before extraction, `veach.spawn{ parent = e }`, `e:parent()`, `e:children()`, `veach.quat` helpers; cameras read `GlobalTransform`. [spec](../specs/transforms/spec.md)
- **VCH-012** DX12 backend — *rhi* — `Dx12Device` on the Agility SDK 1.620: SM 6.6 descriptor heaps, enhanced barriers, root constants, graphics and compute pipelines, DXR 1.1 BLAS/TLAS build and refit, readback, flip-model swapchain. Every shared golden passes on WARP in hosted CI at the default tolerance, plus a 300-frame soak and MCP on DX12. [spec](../specs/dx12-backend/spec.md), [ADR 0031](../adr/0031-dx12-on-warp-in-ci.md)
- **VCH-002** Windows bring-up — *platform* — A windowed run on the hosted Windows runner: SDL window, HWND to the RHI, DX12 swapchain present and resize, clean exit. Real-display presentation is VCH-050.
- **VCH-011** Views and cameras — *render* — `Camera` and `Transform` components (reflected, Scriptable), extracted into reverse-Z `RenderView`s; `FrameContext::view`. The lab triangle renders through a perspective camera in raster and ray-traced form against one shared golden, on Metal and DX12. [spec](../specs/render-views/spec.md)
- **VCH-047** Windows GPU runner script — *process* — Dropped 2026-11-25: there is no Windows machine, and Windows is verified in hosted CI only ([ADR 0030](../adr/0030-windows-verified-in-hosted-ci.md)).
- **VCH-003** MCP server v0 — *agents* — `--mcp` serves MCP over stdio: `query_entities`, `get_component`, `set_component`, `spawn_entity`, `step_frames`, `screenshot` (PNG, headless or windowed), `render_graph_plan`, `gpu_timings`, sandboxed transactional `eval`. Schemas come from reflection; errors carry codes and did-you-mean hints. A test drives the lab through MCP to a golden. `load_scene` moves to VCH-027. [spec](../specs/mcp-server/spec.md)
- **VCH-041** Parameter-block layout checks — *render* — Each shader's parameter block is reflected (Slang, HLSL layout) into `<ns>::layout::<stem>`. `static_assert(rhi::parameters_match<T>(...))` compares names, order, offsets and sizes at compile time. The shader build rejects `float3`/3-row matrices. The hygiene check requires a layout check for every shader with parameters. [ADR 0024](../adr/0024-resource-binding-model.md)

- **VCH-045** Frames in flight — *rhi* — N frames in flight (default 2) on a shared `SubmissionTracker`: monotonic submission ids, per-slot fences, per-frame TLAS instance buffers, and fence-tracked retirement for `destroy()` (absorbs VCH-042). Pooled transients sync with their previous frame (`Barrier::discard`). [ADR 0029](../adr/0029-frame-and-sync-model.md)

- **VCH-010** First light: RHI v0 and render graph v0 — *render* — Metal backend with buffers, textures, samplers, graphics and compute pipelines, BLAS/TLAS build and refit, swapchain, offscreen targets, readback, Metal validation in debug, and GPU errors logged at error level. Bindless through `DescriptorHandle`. The render graph derives order, culling and barriers. A rasterized triangle and a ray-query triangle. [spec](../specs/first-light/spec.md)
- **VCH-013** Golden-image tests — *tools* — `--screenshot`, `veach imgdiff` (NVIDIA FLIP), goldens in Git LFS, an `update-goldens` target, and a skip on unsupported GPUs (`VEACH_REQUIRE_GPU=1` makes it a failure). [spec](../specs/first-light/spec.md)

- **VCH-001** Repository bootstrap — *process* — Skeleton engine: init/shutdown order, fixed phases, Flecs barrel, reflection everywhere, command buffer, Luau API v0 with generated types, asset system with `.meta` ids, SDL3 and headless platforms, null/Metal/DX12-stub RHI, extraction boundary, the lab hello world, CI, and the docs and process files. [specs](../specs/README.md)
