# Rendering: features, shaders, render graph, goldens

How to draw something in Veach today. Design: [ADR 0024](../adr/0024-resource-binding-model.md) (bindless
binding), [ADR 0025](../adr/0025-sync-and-barrier-model.md) (barriers),
[ADR 0026](../adr/0026-shader-compilation.md) (shader compilation), [spec](../specs/first-light/spec.md).
The working example is `apps/lab/src/triangle_feature.cpp`.

## The shape of a frame

```
Extract:  extractors read components   ->  RenderWorld (renderer-owned, rebuilt every frame)
Render:   RenderGraph: "clear" -> every RenderFeature::record() -> "present" / "screenshot"
          compile (order, culling, barriers) -> execute into one CommandList -> submit
```

**Cameras.** A `veach::Camera` on an entity with a `veach::Transform` becomes a `render::RenderView` (view and
projection matrices, reverse-Z, infinite far plane). `FrameContext::view` is the primary one, or null when no
camera is active: a feature that draws in world space multiplies by `view->view_projection` (or unprojects with
`inverse_view_projection` for rays) and falls back to identity. Spec: [render-views](../specs/render-views/spec.md).

Gameplay never sees GPU handles. A module gets data across by registering an **extractor**, which copies
components into a type stored in `RenderWorld`. It draws by adding a **feature**:

```cpp
auto& renderer = veach::services(world).get<veach::render::Renderer>();
renderer.add_extractor("my.thing", [](const veach::World& w, veach::render::RenderWorld& out) {
    w.each([&](const Thing& t) { out.emplace<ThingState>({t.size}); });
});
renderer.add_feature(std::make_unique<ThingFeature>());
```

## Writing a feature

```cpp
class ThingFeature final : public veach::render::RenderFeature {
public:
    std::string_view name() const override { return "my.thing"; }

    veach::Result<> prepare(veach::rhi::Device& device) override {
        // Create pipelines, buffers and acceleration structures once. A failure disables the feature and is logged.
    }

    void record(veach::render::RenderGraph& graph, const veach::render::FrameContext& frame) override {
        const ThingState* state = frame.world.get<ThingState>();
        if (!state) return;
        auto data = graph.import(buffer_, "thing data", veach::rhi::ResourceState::shader_read);
        graph.add_graphics_pass("thing",
            [&](veach::render::PassBuilder& b) {           // declare EVERY access: barriers + residency come from this
                b.read(data);
                b.color_target(frame.scene_color, veach::rhi::LoadOp::load);
            },
            [this, data](veach::render::PassContext& ctx) {
                ctx.cmd.set_pipeline(pipeline_);
                ctx.cmd.set_parameters(Params{ctx.device.descriptor(ctx.buffer(data))});
                ctx.cmd.draw(3);
            });
    }
};
```

Rules:

- **Declare every resource a pass touches**, with the state it is used in. The graph turns those declarations
  into barriers (DX12) and residency (`useResource` on Metal). An undeclared resource works by accident on one
  backend and fails on the other. Metal validation catches it in debug builds.
- **Never issue barriers yourself.** If you need a state the graph can't express, extend the graph.
- Resources you own across frames are `import()`ed with the state they are in *now*. Track it yourself, the
  way `TriangleFeature` tracks its acceleration structures (`undefined` before the first build, `accel_read`
  after). Transient textures (`graph.create_texture`) are allocated and reused by the graph.
- `frame.scene_color` is `rgba8_unorm`, already cleared to `render::background_color`. Draw into it with
  `color_target(..., LoadOp::load)` or write it as a storage texture from compute (`shader_write`).
- See what the graph decided: `--log-level debug` prints `RenderGraph::describe()` (order, culled passes,
  barriers) whenever it changes.

## Depth, culling and integer targets

A graphics pass declares its attachments; the pipeline states how it uses them:

```cpp
auto ids = graph.create_texture({w, h, rhi::Format::rg32_uint, rhi::TextureUsage::color_target, "ids"});
auto depth = graph.create_texture({w, h, rhi::Format::depth32_float, rhi::TextureUsage::depth_target, "depth"});
graph.add_graphics_pass("visibility",
    [&](PassBuilder& b) {
        b.color_target_uint(ids, rhi::LoadOp::clear, {0xFFFFFFFF, 0xFFFFFFFF, 0, 0});  // integers, exact
        b.depth_target(depth, rhi::LoadOp::clear, 0.0f);  // reverse-Z: 0 is the far plane
    }, ...);

rhi::GraphicsPipelineDesc desc;  // ...entries, as before
desc.color_formats = {rhi::Format::rg32_uint};
desc.depth_format = rhi::Format::depth32_float;  // must match the pass's depth attachment
desc.depth_test = true;
desc.depth_write = true;
desc.depth_compare = rhi::CompareOp::greater_equal;  // nearer is greater (reverse-Z)
desc.cull = rhi::CullMode::back;                    // front faces wind counter-clockwise (glTF)
```

- The defaults are no depth test, no depth write and no culling. A pipeline that tests or writes depth without a
  `depth_format` is rejected (`rhi.invalid_pipeline`).
- The depth target is in `depth_write` for the pass; the graph emits its barriers like any other access.
- Integer formats (`rg32_uint`, `r32_uint`) are read with `Load` (never filtered) and cleared with
  `color_target_uint`. Metal clears them exactly; DX12 clears through floats, so integer clear values must be at
  most 2^24 or exactly `0xFFFFFFFF` (anything else is logged as an error).

## GPU timings

The graph times every pass it executes with GPU timestamps (`CommandList::begin_timing`/`end_timing`; Metal
samples at encoder boundaries, DX12 writes timestamp queries). `device.last_frame_timings()` returns the last
completed frame's passes by name, in execution order; MCP `gpu_timings` returns the same after waiting for the
GPU. A device that cannot time passes says why in `FrameTimings::unavailable`. On Metal a pass's time spans its
encoders, so a transfer pass with many builds is first start to last end.

## Shaders

1. Write Slang in `<module>/shaders/<name>.slang`. Reach resources only through `DescriptorHandle<T>` fields in a
   `[[vk::push_constant]] ConstantBuffer<Params>`.
2. Register it in CMake:
   ```cmake
   veach_add_shader(my-target shaders/thing.slang ENTRIES vs_main:vertex fs_main:fragment)
   veach_add_shader(my-target shaders/thing_cs.slang ENTRIES main:compute)
   veach_shader_library(my-target NAMESPACE my_shaders)   # once per target, after the shaders
   ```
3. Include `"my_shaders.hpp"` and pass `&my_shaders::thing()` to the pipeline description. For compute,
   `threads_per_group` must equal the shader's `[numthreads]`.
4. Mirror `Params` with a C++ struct and **check it against the shader's reflected layout** (VCH-041):
   ```cpp
   struct TraceParams { rhi::DescriptorHandle output; rhi::DescriptorHandle scene; std::array<std::uint32_t, 2> size; };
   static_assert(rhi::parameters_match<TraceParams>(my_shaders::layout::thing_cs,
       {VEACH_PARAM(TraceParams, output), VEACH_PARAM(TraceParams, scene), VEACH_PARAM(TraceParams, size)}));
   ```
   `my_shaders::layout::<stem>` is generated from Slang's reflection of the shader. Different field names, order,
   offsets or sizes are a compile error, and the hygiene check fails if a shader with a parameter block has no
   such check. The shader build itself fails on `float3`/`uint3` members and 3-row or 3-column matrices, whose
   layout differs between Metal and HLSL (ADR 0024). Use 8-byte `rhi::DescriptorHandle`s for handles, and never
   leave one unwritten (handle 0) where a shader might read it, even behind a flag: the compiler may load the
   descriptor before the branch. Point absent resources at `device.null_descriptor(kind)` (ADR 0039). Keep raster and compute entry points that need different parameters
   in separate `.slang` files.

macOS builds compile to `.metallib`. Windows builds compile every entry point to DXIL, and the Windows CI job
renders the goldens with it on WARP, so non-portable Slang fails CI either way.

## Scenes, materials, lights, and the two renderers

- **Scenes:** `SceneInstance { path = "assets/x.gltf" }` spawns the glTF as child entities (`Transform`,
  `MeshInstance`, lights). Import and cooking: [ADR 0032](../adr/0032-asset-import-and-cooking.md).
- **Materials** are Slang types conforming to `IMaterial` (`shaders/veach/material.slang`). Every function has
  a default (glTF metallic-roughness), and a material overrides only what it changes:

  ```slang
  import veach.material;
  struct GlowMaterial : IMaterial {
      override float3 evalEmission(MaterialContext c) { return standardEmission(c) * stripes(c.surface.uv); }
  };
  ```

  Register it with `veach_add_material(<app> GlowMaterial shaders/glow_material.slang [EMISSIVE])`, then
  `veach_add_scene_shaders(<app> NAMESPACE <ns>)`. A glTF material selects it with `"extras": {"veach_material":
  "GlowMaterial"}`. Renderers call `evaluateSurface(kind, context)` and never read material records directly
  ([ADR 0033](../adr/0033-material-model.md)).
- **Lights:** `DirectionalLight` (lux), `PointLight` and `SpotLight` (candela), shining down −Z, and emissive
  surfaces (nits). `PointLight.radius` and `SpotLight.radius` (meters) and `DirectionalLight.angle` (angular
  diameter, radians) give them a size and soft shadows in both renderers, at the same intensity
  ([soft-shadows spec](../specs/soft-shadows/spec.md)). `Camera.ev100` sets exposure, or derives it from `aperture`, `shutter` and `iso` when 0; a
  display pass applies it and the AgX tonemapper to the linear radiance renderers produce
  ([ADR 0038](../adr/0038-exposure-and-tonemapping.md)).
- **One feature, two renderers.** The app adds `render::SceneRenderer(SceneShaders{<ns>::path_tracer(),
  <ns>::realtime_visibility(), <ns>::realtime_shade()}, <ns>::material_kinds, assets)`. It keeps the scene on the
  GPU (meshes with their meshlets, BLASes and a TLAS, material, light and emitter records) and draws each view
  with the renderer its camera asks for. Both share the surface reconstruction, BRDF and light sampling
  (`veach/scene.slang`, `veach/brdf.slang`, `veach/lights.slang`), so a material renders the same in both
  ([ADR 0037](../adr/0037-realtime-renderer-visibility-buffer.md)).
- **The real-time renderer** draws a camera without a `PathTracer` ([temporal spec](../specs/temporal-direct-lighting/spec.md)),
  at the render resolution (`Upscaling.render_scale`, default 0.75) with sub-pixel jitter:
  1. `visibility`: meshlet triangles into the visibility buffer (ADR 0037);
  2. `surface`: the G-buffer, analytic motion vectors and the disocclusion decision every temporal pass shares
     (`veach/temporal.slang`, ADR 0040, ADR 0045);
  3. ReSTIR DI (ADR 0041): `light_candidates` (12) alias-table samples over point, spot and emissive lights,
     temporal and `spatial_samples` (2) spatial reuse with pairwise MIS, then `shade` with one shadow ray per
     pixel, plus one per directional light, evaluated exactly (ADR 0046), the ambient stopgap and emission;
  4. the denoiser (ADR 0042): demodulated diffuse and specular, temporal accumulation, variance, five a-trous
     iterations, remodulation;
  5. the upscaler (ADR 0043): MetalFX's temporal scaler where the device has it (`Upscaling.mode = "automatic"`),
     Veach's TAAU otherwise (`"temporal"`, what goldens pin so Metal and DX12 share one image); then AgX.

  `Upscaling { mode = "none" }` renders at full resolution without jitter or upscaling. History is per camera and
  resets on a resize, a mode change or a cut (more than 2 m or 45 degrees in a frame). `RealtimeLighting.view`
  shows `noisy` (before the denoiser), `motion`, `history` (green: last frame's surface found, red: disoccluded)
  or `accumulated` (the undenoised lighting averaged while nothing changes: what bias checks compare). Example:
  `scripts/cornell_box_realtime.luau`.
- **The reference path tracer** renders a camera that has a `PathTracer { samples_per_frame, max_bounces, mode,
  jitter }`. It accumulates while nothing changes and restarts on any change. `mode` 1 computes direct light
  only, which is what the real-time renderer estimates; mode 3 adds the sky on the first hit's diffuse lobe (the
  real-time ambient stopgap's term); `tools/bench/compare_direct.py` measures one against the other per scene
  (`--sky` for mode 3). Example: `scripts/cornell_box.luau`. It counts what should never happen and fails the run:
  pixels whose accumulation reads back wrong (`render.history_lost`) and NaN or infinite samples
  (`render.nonfinite_samples`).
- **The sky** ([ADR 0050](../adr/0050-physical-atmosphere.md)): a `Sky` component on any entity, physical by
  default. Its lookup tables (transmittance, multiple scattering, sky view, aerial perspective) are built by the
  scene renderer's passes and read by both renderers: the real-time background, ambient stopgap and haze, and the
  path tracer's escaped rays. Without a `Sky` entity the sky follows the scene's brightest directional light; with
  one, it drives the sun from `time_of_day`. Closed test scenes set `Sky { mode = "none" }` (a warning says so when
  the scene has lights). Example: `scripts/bistro_sunset.luau`.
- **GPU timings:** `tools/bench/frame_times.py` reports the frame and each pass over MCP (`gpu_timings`).
- **Looking around:** the lab runs `scripts/dev/fly_camera.luau` after every scene (`"dev_scripts"` in its
  `veach.project.json`), so any windowed run flies its active camera with W/A/S/D, Q/E, the arrows and shift:
  `veach-lab --rhi metal --window --script scripts/bistro.luau`. It writes the camera only while a key is held,
  so goldens, benchmarks and replays keep the scene's view.

## Screenshots and golden images

```sh
veach-lab --rhi metal --headless --frames 3 --script scripts/triangle_raytraced.luau --screenshot out.png
build/debug/tools/veach-cli/veach imgdiff tests/golden/triangle_raytraced.png out.png --max-mean 0.01 --diff diff.png
```

- `--screenshot` captures the last frame's scene color (needs `--frames N` and a GPU RHI).
- `veach imgdiff` compares with NVIDIA FLIP. On failure it writes a magma error map with `--diff`. Mean FLIP 0
  means identical; up to about 0.01 is invisible.
- Golden tests are CTest tests named `golden: ...` (macOS). They **skip**, never pass, on GPUs without Metal 3 or
  ray tracing. Set `VEACH_REQUIRE_GPU=1` on machines that must run them.
- On Metal the golden image is rendered with `MTL_SHADER_VALIDATION=0`, and a second run with it on (the debug
  default) must exit 0: under shader validation, ray queries that read instance data after a hit or trace twice
  return shadows that vary from run to run. Take screenshots you compare the same way.
- **Regenerating goldens is a deliberate act:** `cmake --build --preset debug --target update-goldens` writes
  `tests/golden/*.png` (stored in Git LFS). Look at the images before committing, and say why they changed in
  the commit message.

## Adding a golden test

Add a lab script that sets up the scene, then `veach_golden_test(...)` calls in `tests/CMakeLists.txt`, one per
backend (`FRAMES n` for accumulating scenes), plus a line in the `update-goldens` target. Generate the golden on
Metal, inspect it, and commit it.
