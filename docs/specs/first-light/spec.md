# First light: RHI v0, render graph v0, golden images

**Status:** Implemented (macOS). Metal goldens run in the pre-push gate, not in CI ([ADR 0034](../../adr/0034-pre-push-gate-no-gpu-runner.md)).
**Area:** render
**Owner:** dev lead

## Summary

Veach's first images, rasterized and ray traced, and tests that prove them. The Metal backend of the RHI gets real resources,
pipelines, acceleration structures, command encoding, swapchains, offscreen targets and readback. A render
graph derives pass order and barriers. The lab draws the same triangle twice, once rasterized and once by a
compute shader using ray queries against a TLAS, and golden-image tests compare both with committed references.

## Motivation

Everything that renders goes through these seams, the DX12 backend included. Getting the
binding model ([ADR 0027](../../adr/0027-resource-binding-model.md)), the barrier model
([ADR 0029](../../adr/0029-render-graph-sync-and-barriers.md)) and the shader pipeline
([ADR 0009](../../adr/0009-shader-compilation.md)) right on day one, and proving them with images agents can
check, is cheaper than retrofitting them.

## Requirements

- MUST compile every shader from Slang at build time: to a `.metallib` on macOS, and to DXIL on every Windows
  build, even though there is no DX12 backend yet. No hand-written MSL.
- The RHI MUST provide buffers, textures, samplers, graphics and compute pipelines, BLAS/TLAS build and refit,
  render/compute/transfer command encoding, swapchains from a `NativeWindowHandle`, offscreen render targets,
  GPU→CPU readback, and bindless descriptors (`DescriptorHandle`).
- The DX12 backend MUST implement the whole `rhi::Device` interface as stubs, compiled on every build
  (`static_assert(!std::is_abstract_v<Dx12Device>)`).
- The render graph MUST derive the pass order from declared accesses, cull passes whose results are unused, emit
  barriers per ADR 0029 (including write-after-write and final states of imported resources), declare residency,
  and reject impossible accesses with `render.graph_invalid`. `describe()` prints the plan as text.
- The renderer MUST run unchanged on the null RHI (headless CI), and modules MUST extend it through
  `RenderFeature` and extractors only.
- Debug builds MUST enable Metal API validation. GPU errors MUST be logged at error level (`rhi.gpu_error`), so
  the run exits with code 1.
- Every app MUST accept `--screenshot <path.png>`: it writes the last frame (`--frames N`) as a PNG, and works
  with `--headless`.
- Golden-image tests MUST compare perceptually (NVIDIA FLIP, mean error ≤ 0.01), store goldens in Git LFS, and
  offer an explicit regeneration command. On a GPU that cannot run them they MUST report SKIPPED, never pass;
  `VEACH_REQUIRE_GPU=1` turns that skip into a failure.

## API sketch

```cpp
class TriangleFeature final : public veach::render::RenderFeature {
    veach::Result<> prepare(veach::rhi::Device& device) override;   // pipelines, BLAS/TLAS, buffers
    void record(veach::render::RenderGraph& graph, const veach::render::FrameContext& frame) override {
        auto tlas = graph.import(tlas_, "tlas", built_ ? accel_read : undefined);
        graph.add_compute_pass("trace",
            [&](PassBuilder& b) { b.read(tlas, accel_read); b.write(frame.scene_color, shader_write); },
            [&](PassContext& ctx) {
                ctx.cmd.set_pipeline(trace_);
                ctx.cmd.set_parameters(TraceParams{ctx.device.descriptor(ctx.texture(frame.scene_color), View::storage),
                                                   ctx.device.descriptor(ctx.accel(tlas)), ...});
                ctx.cmd.dispatch((w + 7) / 8, (h + 7) / 8);
            });
    }
};
```

```sh
veach-lab --rhi metal --headless --frames 3 --script scripts/triangle_raytraced.luau --screenshot out.png
build/debug/tools/veach-cli/veach imgdiff tests/golden/triangle_raytraced.png out.png --max-mean 0.01
cmake --build --preset debug --target update-goldens      # regenerate goldens on purpose
```

## Acceptance criteria

- **render.AC1**: The graph orders passes by dependencies (it groups graphics work ahead of an independent
  compute pass) and culls a pass whose output nobody reads.
- **render.AC2**: Barriers match declared accesses: `undefined → shader_write`, `shader_write → shader_write`
  (UAV), `shader_write → color_target`, and the final `color_target → present` of an imported image.
- **render.AC3**: Execution opens the right encoder per pass kind and calls `use()` for every declared access.
- **render.AC4**: Reading a transient that is never written, or a color target in a compute pass, fails with
  `render.graph_invalid`.
- **render.AC5**: A headless run on the null RHI executes the whole renderer and logs no errors.
- **first-light.AC1**: `golden: triangle (raster)`: the rasterized triangle matches `tests/golden/triangle_raster.png`.
- **first-light.AC2**: `golden: triangle (raytraced)`: the ray-query triangle matches
  `tests/golden/triangle_raytraced.png`. The TLAS is built on frame 1 and refit on frames 2 and 3, so refit is
  exercised.
- **first-light.AC3**: `golden: raster and ray-traced goldens agree`: the two committed goldens differ by a mean
  FLIP error ≤ 0.005. Measured: 0.00000, worst pixel 0.013.
- **first-light.AC4**: Slang → DXIL compiles every shader on Windows CI. Slang → metallib compiles every shader on
  macOS CI.
- **first-light.AC5**: Manual check, macOS: `veach-lab --window --rhi metal --script scripts/triangle_raytraced.luau`
  presents through the swapchain (`present` pass, swapchain `present → color_target → present`) with Metal
  validation on and no errors. Verified 2026-11-24 on an Apple M5 Max: 900 frames, clean shutdown.

## Out of scope

- The DX12 implementation (VCH-012), depth buffers, a camera (VCH-011), transient aliasing, async compute, and
  layout validation of parameter blocks against Slang reflection (VCH-041). Frames in flight and deferred
  destruction landed later, in VCH-045 ([ADR 0030](../../adr/0030-frame-and-sync-model.md)).

## Open questions

- **CI GPU.** GitHub's hosted macOS runners expose an "Apple Paravirtual device" (no Metal 3, no ray tracing,
  tier-1 argument buffers), so the GPU goldens skip there. Decided: the pre-push gate runs them on a real Mac
  ([ADR 0034](../../adr/0034-pre-push-gate-no-gpu-runner.md)).

## Links

- ADRs: [0004 RHI](../../adr/0004-own-rhi-not-sdl-gpu.md), [0027 binding](../../adr/0027-resource-binding-model.md),
  [0029 barriers](../../adr/0029-render-graph-sync-and-barriers.md), [0009 shaders](../../adr/0009-shader-compilation.md)
- Backlog: VCH-010, VCH-013 (done), VCH-012, VCH-041, VCH-043, VCH-044 (won't do, ADR 0034)
- Guide: [rendering](../../guides/rendering.md)
- Code: `engine/rhi/`, `engine/render/`, `apps/lab/src/triangle_feature.cpp`, `apps/lab/shaders/`, `tests/golden/`
