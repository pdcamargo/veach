# Rendering: features, shaders, render graph, goldens

How to draw something in Veach today. Design: [ADR 0027](../adr/0027-resource-binding-model.md) (bindless
binding), [ADR 0029](../adr/0029-render-graph-sync-and-barriers.md) (barriers),
[ADR 0009](../adr/0009-shader-compilation.md) (shader compilation), [spec](../specs/first-light/spec.md).
The working example is `apps/lab/src/triangle_feature.cpp`.

**Where the renderer is going.** The real-time renderer is a stylized PBR rasterizer with no real-time GI or path
tracing ([ADR 0039](../adr/0039-realtime-renderer-is-raster-pbr.md)). It is a GPU-driven visibility buffer
([ADR 0060](../adr/0060-gpu-driven-visibility-buffer.md)); its core is described below ([ADR 0093](../adr/0093-gpu-driven-core.md),
[spec](../specs/gpu-driven-renderer/spec.md)). Real-time features never require hardware ray tracing
([ADR 0003](../adr/0003-platforms-and-hardware-target.md)); only photo mode uses it.

## The shape of a frame

```
Extract:  extractors read components   ->  RenderWorld (renderer-owned, rebuilt every frame)
Render:   RenderGraph: "clear" -> every RenderFeature::record() -> "present" / "screenshot"
          compile (order, culling, barriers) -> execute into one CommandList -> submit
```

**Cameras.** A `veach::Camera` on an entity with a `veach::Transform` becomes a `render::RenderView` (view and
projection matrices, reverse-Z, infinite far plane). `FrameContext::view` is the primary one, or null when no
camera is active: a feature that draws in render space multiplies by `view->view_projection` (or unprojects with
`inverse_view_projection` for rays) and falls back to identity. Spec: [render-views](../specs/render-views/spec.md).

**Render space, not world space.** Positions are doubles in reference frames ([frames](../specs/frames/spec.md),
ADR 0045). Extraction picks one render origin per frame, the primary camera's position in its frame rounded to a 64 m
grid, and puts every view, draw and light relative to it, in that frame's axes (`RenderWorld::space`), computing in
double and casting to float32. So the GPU never sees a large number, and a flat scene near the origin renders exactly
as before. Use `view.height_in_frame` and `view.up` for altitude-like quantities, never `view.position.y`. A module that
draws its own content gets it into render space with `render::RenderSpaceMap(world, out.space)` (`matrix(pose)`,
`point(frame, p)`); keep GPU vertices relative to small local origins and rebase only the origins.

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
- **MSAA, Metal only (spike SP-16).** A texture with `sample_count` 2, 4 or 8 lives in tile memory: usable only as a
  color or depth target of one pass, never loaded, stored, sampled or copied. `PassBuilder::resolve(ms, target)`
  averages its samples into a single-sampled `target` at the end of the pass (depth: the farthest sample). Pipelines
  set the same `sample_count`, and `alpha_to_coverage` for foliage. DX12 rejects it (`rhi.not_implemented`); the
  real-time renderer does not use it ([ADR 0060](../adr/0060-gpu-driven-visibility-buffer.md),
  [SP-16](../research/spikes/sp-16-visibility-vs-forward.md)).

## Indirect draws and dispatches

A compute pass writes the arguments; a later pass draws or dispatches with them
([spec](../specs/gpu-driven-rhi/spec.md), [ADR 0089](../adr/0089-indirect-work-and-fixed-latency-readback.md)):

```cpp
// BufferUsage::indirect for arguments (storage too, so a shader can write them); BufferUsage::index for indices.
graph.add_compute_pass("cull", [&](PassBuilder& b) { b.read(instances); b.write(args); }, ...);
graph.add_graphics_pass("draw",
    [&](PassBuilder& b) {
        b.read(args, rhi::ResourceState::indirect_argument);
        b.read(indices, rhi::ResourceState::index_read);   // indexed draws only
        b.color_target(color);
    },
    [=](PassContext& ctx) {
        ctx.cmd.set_pipeline(pipeline);
        ctx.cmd.set_parameters(params);
        ctx.cmd.draw_indexed_indirect(ctx.buffer(indices), ctx.buffer(args), /*offset*/ 0);
    });
// In a compute pass: ctx.cmd.dispatch_indirect(ctx.buffer(args), offset) after b.read(args, indirect_argument).
```

- The shader writes `rhi::DispatchIndirectArgs`, `DrawIndirectArgs` or `DrawIndexedIndirectArgs` as `uint`s into a
  `RWStructuredBuffer<uint>`; they are both APIs' own layouts.
- `draw_indirect(args, offset, draw_count, stride)` draws `draw_count` records; the count is the CPU's. To vary the
  amount of work on the GPU, compact the survivors into a list and write `instance_count`, or zero the records you
  skip. There are no count buffers (Metal has none).
- Keep `first_instance` and `vertex_offset` 0: Metal adds them to `SV_InstanceID` and `SV_VertexID`, D3D12 does
  not. Put offsets in your own data.
- Offsets and strides are multiples of 4; every backend, null included, asserts that the records fit.

## Reading GPU results back

`FrameContext::readbacks` copies a buffer to the CPU without waiting. The bytes reach your callback exactly
`frames_in_flight` frames later (2 by default), on every backend:

```cpp
frame.readbacks->read(graph, counts, sizeof(std::uint32_t), [this](Result<render::ReadbackData> data) {
    if (data) {
        std::memcpy(&visible_, data->bytes.data(), sizeof(visible_));  // data->frame: the frame that copied it
    }
});
```

- The source buffer needs `BufferUsage::copy_src`; the queue adds a transfer pass that reads it in `copy_src`.
- Callbacks run inside `Renderer::render()`. Copy the bytes into your module's own state and read them from a
  system later; never write the World from a callback. GPU results are visual or diagnostic only: never feed them
  into canonical simulation (ADR 0043).
- On the null RHI the bytes are zeros. A frame whose graph fails to compile drops its readbacks
  (`render.readback_dropped`).
- `Device::is_complete(submission)` answers "has this finished?" without waiting, for code outside the frame loop.

## Texture kinds, updates, blending and samplers

[Spec](../specs/rhi-textures/spec.md), [ADR 0090](../adr/0090-rhi-texture-kinds-updates-and-raster-state.md).

```cpp
// A froxel volume written in compute and sampled as a Texture3D.
auto volume = device.create_texture({.width = 160, .height = 90, .format = rhi::Format::rgba16_float,
                                     .usage = rhi::TextureUsage::storage | rhi::TextureUsage::sampled,
                                     .debug_name = "froxels", .type = rhi::TextureType::d3, .depth = 32});
params.out = ctx.device.descriptor(ctx.texture(v), rhi::View::storage);   // RWTexture3D<float4>
params.volume = ctx.device.descriptor(ctx.texture(v));                    // Texture3D<float4>

// Shadow cascades: a 2D array of depth, one layer per pass, PCF through a comparison sampler.
b.depth_target(cascades, rhi::LoadOp::clear, 0.0f, {.layer = i});
desc.depth_bias = {.constant = -2, .slope = -1.5f};   // reverse-Z: negative pushes away from the light
auto pcf = device.create_sampler({.compare = rhi::CompareOp::greater_equal});  // SamplerComparisonState

// Streaming: stage bytes and copy them into a box of one mip, in a transfer pass the graph orders.
render::upload_texture(graph, *frame.uploads, layers, {.mip = 0, .base_layer = 2, .layer_count = 1}, bytes);

// Transparency and terrain.
desc.blend = {rhi::BlendMode::premultiplied};
auto terrain = device.create_sampler({.address = rhi::AddressMode::repeat,
                                      .max_anisotropy = rhi::terrain_anisotropy});
```

- **Types.** `d2`, `d2_array`, `d3`, `cube` (6 layers: +X, -X, +Y, -Y, +Z, -Z) and `cube_array`. Read descriptors
  have the type's Slang type; storage descriptors are `RWTexture2D`, `RWTexture3D`, or `RWTexture2DArray` for arrays
  and cube faces. In Slang a depth texture is `Texture2D<float>` and `SampleCmp` works on it on both backends.
- **Parts.** `descriptor(texture, view, {.base_mip, .mip_count, .base_layer, .layer_count})` views a run of mips and
  layers; storage views see one mip. Point absent textures at `null_descriptor(DescriptorKind::texture_3d)` and the
  other kinds, as for any handle (ADR 0027).
- **Data.** Initial data and uploads are tightly packed per mip in KTX2 order: layers, then depth slices, then rows
  (`rhi::mip_bytes`). `upload_texture` regions with zero extents reach the edge of the mip; regions uploaded to one
  texture in one frame must not overlap. The ring's memory is reused when its frame slot comes round, so stage only
  in the frame that copies.
- **Barriers.** The graph keeps a state per mip. `graph.mips(texture, first, count)` is a resource of its own over
  those mips: a pass can read mip n - 1 while it writes mip n (a HiZ pyramid, a mip chain), and passes touching
  different mips are not ordered against each other. Shaders reach them through
  `descriptor(ctx.texture(view), view, ctx.range(view))`. A pass may not touch a mip twice (through the texture and a
  view of it). A texture whose mips end the graph in different states moves to its final state, or mip 0's. Layers
  are still tracked together: a pass writing one layer and a pass reading another are ordered.
- **Blend modes.** `alpha` is straight alpha, `premultiplied` expects `rgb * a` from the shader, `additive` adds color
  and keeps the destination's alpha. Integer targets cannot blend.

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
   layout differs between Metal and HLSL (ADR 0027). Use 8-byte `rhi::DescriptorHandle`s for handles, and never
   leave one unwritten (handle 0) where a shader might read it, even behind a flag: the compiler may load the
   descriptor before the branch. Point absent resources at `device.null_descriptor(kind)` (ADR 0027). Keep raster and compute entry points that need different parameters
   in separate `.slang` files.

macOS builds compile to `.metallib`. Windows builds compile every entry point to DXIL, and the Windows CI job
renders the goldens with it on WARP, so non-portable Slang fails CI either way.

## Scenes, materials, lights, and the two renderers

- **Scenes:** `SceneInstance { path = "assets/x.gltf" }` spawns the glTF as child entities (`Transform`,
  `MeshInstance`, lights). Import and cooking, meshlets included: [ADR 0011](../adr/0011-gltf-import-and-cooking.md).
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
  ([ADR 0035](../adr/0035-material-model.md)).
- **Lights:** `DirectionalLight` (lux), `PointLight` and `SpotLight` (candela), shining down −Z, and emissive
  surfaces (nits) ([ADR 0036](../adr/0036-lights-in-photometric-units.md)). `PointLight.radius`, `SpotLight.radius`
  (meters) and `DirectionalLight.angle` (angular diameter, radians) give a light a size: soft shadows in photo mode.
  In real time the two brightest directional lights cast shadow maps whose penumbrae follow `angle` (below); every
  other light is an unshadowed point. `Camera.ev100` sets exposure, or derives it from
  `aperture`, `shutter` and `iso` when 0; the display pass applies it and the AgX tonemapper
  ([ADR 0037](../adr/0037-exposure-and-agx.md)).
- **One feature, two renderers.** The app adds `render::SceneRenderer(SceneShaders{<ns>::path_tracer(),
  <ns>::realtime_visibility(), <ns>::realtime_shade_bins()}, <ns>::material_kinds, assets)`. It keeps the scene on the
  GPU (meshes with their meshlets and meshlet bounds, the GPU scene's instance records, material and light records) and
  draws each view with the renderer its camera asks for.
  Both share surface reconstruction and the BRDF (`veach/scene.slang`, `veach/brdf.slang`), so a material is the same
  surface in both. Sidedness means something: a double-sided material is a thin sheet whose triangle is the surface; a
  single-sided one bounds a solid whose vertex normals are the surface, so a triangle of it seen edge-on or from behind
  (a fold, a Transvoxel seam) shades like the surface around it
  ([VCH-108](../research/notes/vch-108-pinholes.md)).
- **The real-time renderer** ([spec](../specs/gpu-driven-renderer/spec.md), [ADR 0093](../adr/0093-gpu-driven-core.md))
  draws a camera without `PhotoMode`, GPU-driven:
  1. **The GPU scene** (`gpu_scene.cpp`): every drawn mesh's `InstanceRecord` in one persistent buffer, in render
     space, with last frame's transform; only changed records are uploaded (a still scene uploads nothing).
  2. **Culling, phase 1** (`gpu_cull.slang`): instances, then meshlets in tasks of 64 (one-meshlet instances inline):
     the frustum, the normal cone (single-sided materials, uniform scale), last frame's HiZ pyramid. Survivors go to
     eight compacted draw lists (opaque and alpha-tested, by meshlet size); the occluded go to recheck lists.
  3. `visibility`: the lists drawn indirectly, opaque then alpha-tested, into the `rg32_uint` visibility buffer
     (`{instance, meshlet << 7 | triangle}`), reverse-Z depth and `rg16_float` motion (previous minus current UV).
  4. `hiz 0..n` (`hiz.slang`): the pyramid of that depth, a pass per mip (`RenderGraph::mips`).
  5. **Culling, phase 2**: the rechecked instances and meshlets against the new pyramid; `visibility (late)` draws what
     became visible, so a camera that jumps never shows a hole.
  6. `classify` (`classify.slang`): each 8x8 tile into the bins it shows (the sky, then one per material kind), and
     the sky's motion; `shade`: one indirect dispatch per bin, each its own entry point
     (`realtime_shade_bins.slang`, generated by `veach_shade_bins()`), lighting with every punctual light (the
     suns through their shadow maps), the sky's ambient light and emission, the sky behind geometry and aerial
     perspective in front;
  7. `display`: AgX.

  `FrameContext::blackboard` hands `visibility`, `depth`, `motion` and `hdr_color` to features recorded after it. What
  culling did (`render::CullStats`: instances visible and rechecked, meshlets culled, occluded and drawn per phase) is
  read back each frame (`SceneRenderer::cull_stats`, MCP `gpu_timings`' `culling`). Lighting is basic on purpose (no
  indirect light, no reflections) and uses no ray tracing: `VEACH_DISABLE_RAY_TRACING=1` renders the same image.
  Example: `scripts/flight_helmet_realtime.luau`.
- **Sun shadows** ([spec](../specs/sun-shadows/spec.md), [ADR 0095](../adr/0095-sun-shadows.md)): the brightest
  directional lights (at most two) cast cascaded shadow maps, before shading (`shadows.cpp`). A camera's `SunShadows`
  sets how many suns cast (`suns`, 0 turns them off), where the last cascade ends (`distance`, meters, default 1000),
  how many cascades (`cascades`, 1 to 4, each a quarter of the next) and their size (`resolution`, default 2048).
  Each cascade is the bounding sphere of its slice of the frustum, snapped to its texels in the render frame (no
  shimmer as the camera moves or render space changes cell), culled by `gpu_cull.slang` with `cull_shadow_view` (no
  occlusion, no normal cones; casters must be big enough and their shadow must reach the camera's slice) into its own
  draw lists, and drawn depth-only into a layer of one `depth32_float` 2D array (`shadow_vs`, `shadow_fs_masked` in
  `realtime_visibility.slang`). Shading calls `light_shadow` (`veach/shadows.slang`) once per light: the cascade by
  view depth, a blocker search, then comparison-sampler PCF as wide as the light's angular radius makes the
  penumbra; the last tenth of each cascade blends into the next and the last fifth of the distance fades out.
  Beyond `distance` only a surface's own facing darkens it. `CullStats::shadow_meshlets_drawn` reports each
  cascade's meshlets.
- **Photo mode** ([ADR 0041](../adr/0041-photo-mode.md), [spec](../specs/photo-mode/spec.md)) path traces a camera that
  has `PhotoMode { samples_per_frame, max_bounces }`: the reference path tracer (`shaders/path_tracer.slang`), with
  BLASes and a TLAS built only for it. It accumulates while nothing changes and restarts on any change; no denoiser.
  It fails the run on accumulation that reads back wrong (`render.history_lost`) and on NaN or infinite samples
  (`render.nonfinite_samples`). On a device without hardware ray tracing it logs `rhi.backend_unavailable` and the
  camera renders in real time. MCP `photo_mode { samples }` renders a reference image of the current state.
  Example: `scripts/cornell_box.luau`.
- **The sky** ([ADR 0038](../adr/0038-physical-sky.md)): a `Sky` component on any entity, physical by default. Its
  lookup tables (transmittance, multiple scattering, sky view, sky irradiance) and aerial-perspective volume are built by the
  scene renderer's passes and read by both renderers: the real-time background, ambient light and haze, and photo
  mode's escaped rays. Without a `Sky` entity the sky follows the scene's brightest directional light; with one, it
  drives the sun from `time_of_day`. Closed test scenes set `Sky { mode = "none" }` (a warning says so when the scene
  has lights). Example: `scripts/bistro_sunset.luau`.
- **A planet's atmosphere** ([spec](../specs/planet-atmosphere/spec.md), [ADR 0092](../adr/0092-aerial-perspective-follows-the-air.md)):
  a module fills `ResolvedSky::planet` (`render::PlanetSky`: the medium, the center, up to four suns and four
  occluder spheres that eclipse them per sample). Aerial perspective is a 3D volume (one cell per 16 × 16 pixels, 32
  slices) whose slices span each ray's own path through the air, so it serves orbit and the ground alike; a shading
  path applies it with `aerial_perspective(sky, direction, uv, distance)` and occludes the sky's ambient light with
  `sky_ambient_radiance(sky, distance)`. A camera's `SkyVisibility { visibility, reach }` keeps haze and sky light
  out of caves and interiors; `physics::sky_visibility` measures it from static colliders. The tables that hold
  luminance are float32 (twilight is millions of times fainter than day, under half floats' range), and every march
  lights each step for its part outside the planet's shadow ([ADR 0094](../adr/0094-twilight-precision.md)); a new
  march over the air should do the same (`planet_shadows`, `planet_in_scatter`).
- **The space backdrop** ([ADR 0091](../adr/0091-space-backdrop.md), [spec](../specs/space-backdrop/spec.md)): stars,
  the galactic band and nebulae beyond every body, behind the sky and through its air. Data in
  `ResolvedSky::backdrop` (`render::Backdrop`, from a module's extractor or the `SpaceBackdrop` component's `seed`;
  `generate_backdrop(seed, GalaxyView)` makes one). The band and nebulae are baked once into a 4096 x 2048 lat-long
  in galactic coordinates (`space backdrop bake`, when they change); the stars are drawn per pixel by the
  `space backdrop` pass after shading (0.3 ms at 1440p on the M5 Max). It has its own exposure
  (`SpaceBackdrop.exposure_compensation`, `exposure_follow`), whose boost the sky in front hides where it outshines
  the backdrop in the world and in the image
  ([rule](../specs/space-backdrop/spec.md#the-sky-in-front-hides-the-boost): starless by day, whole in space), and
  is left out of auto-exposure's metering: the radiance target's alpha is the share of a pixel that is metered, so
  a pass that writes the HDR target before metering writes alpha 1. Example: `scripts/space_backdrop.luau` (deep
  space, no meshes: a scene with a backdrop draws).
- **Meshes made at run time** ([spec](../specs/runtime-meshes/spec.md), [ADR 0072](../adr/0072-runtime-meshes.md)):
  terrain chunks and other generated geometry go through the `render::MeshStore` service, not the cook step.
  `add(MeshData{vertices, indices, triangle_slots, materials})` validates the data (`render.mesh_invalid`), builds
  meshlets with the cooker's `build_meshlets` and returns an id; `add_material(MaterialAsset)` makes a material the
  same way. An entity with `RuntimeMeshInstance { mesh }` and a `Transform` draws it in both renderers. `update(id,
  data)` re-uploads it once, the next frame it is drawn, and retires the old buffers; `remove(id)` frees its GPU
  memory. The store owns the mesh, not the component. Ids are `AssetId`s that never collide with assets
  (`render::is_runtime_id`). Example: the lab's `RuntimeMeshDemo` (`apps/lab/src/runtime_mesh_demo.cpp`) and
  `scripts/runtime_mesh_realtime.luau`.
- **Looking around:** the lab runs `scripts/dev/fly_camera.luau` after every scene (`"dev_scripts"` in its
  `veach.project.json`), so any windowed run flies its active camera with W/A/S/D, Q/E, the arrows and shift:
  `veach-lab --rhi metal --window --script scripts/bistro.luau`. It writes the camera only while a key is held, so
  goldens and replays keep the scene's view.

## Screenshots and golden images

```sh
veach-lab --rhi metal --headless --frames 3 --script scripts/flight_helmet_realtime.luau --screenshot out.png
build/debug/tools/veach-cli/veach imgdiff tests/golden/flight_helmet_realtime.png out.png --max-mean 0.01 --diff diff.png
```

- `--screenshot` captures the last frame's scene color (needs `--frames N` and a GPU RHI).
- `veach imgdiff` compares with NVIDIA FLIP. On failure it writes a magma error map with `--diff`. Mean FLIP 0
  means identical; up to about 0.01 is invisible.
- Golden tests are CTest tests named `golden: ...` (Metal on macOS, DX12/WARP on Windows CI). They **skip**, never
  pass, on GPUs without Metal 3, and photo-mode ones also on GPUs without ray tracing. Set `VEACH_REQUIRE_GPU=1` on
  machines that must run them (the pre-push gate does).
- On Metal the golden image is rendered with `MTL_SHADER_VALIDATION=0`, and a second run with it on (the debug
  default) must exit 0: under shader validation, ray queries that read instance data after a hit or trace twice
  return shadows that vary from run to run. Take screenshots you compare the same way.
- **Regenerating goldens is a deliberate act:** `cmake --build --preset debug --target update-goldens` writes
  `tests/golden/*.png` (stored in Git LFS). Look at the images before committing, and say why they changed in
  the commit message.

## Adding a golden test

Add a lab script that sets up the scene, then `veach_golden_test(...)` calls in `tests/CMakeLists.txt`, one per
backend (`FRAMES n` for photo mode, `NO_RAY_TRACING` to render with ray tracing disabled), plus an entry in the
`update-goldens` list. Generate the golden on
Metal, inspect it, and commit it.
