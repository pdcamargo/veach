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
4. Mirror `Params` with a C++ struct. Follow the layout rules in ADR 0024: 8-byte `rhi::DescriptorHandle`s,
   16-byte alignment for `float4`/`float4x4`, no `float3`/`uint3` members, and a `static_assert` on the size.
   Keep raster and compute entry points that need different parameters in separate `.slang` files.

macOS builds compile to `.metallib`. Windows builds compile every entry point to DXIL, so non-portable Slang
fails Windows CI even before the DX12 backend exists.

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
- **Regenerating goldens is a deliberate act:** `cmake --build --preset debug --target update-goldens` writes
  `tests/golden/*.png` (stored in Git LFS). Look at the images before committing, and say why they changed in
  the commit message.

## Adding a golden test

Add a lab script that sets up the scene, then a `golden: ...` test in `tests/CMakeLists.txt` that follows the
existing `foreach(_mode raster raytraced)` block, plus a line in the `update-goldens` target. Generate the
golden, inspect it, and commit it.
