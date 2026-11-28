# Extending Veach

How to add a component, system, module, Luau binding, engine subsystem, asset loader, MCP tool,
error code, extractor, material or generation stage, and how to run work in parallel. App and game code uses `veach::` names only and includes
`<veach/veach.hpp>`; engine code may use Flecs directly. Background:
[ARCHITECTURE.md](../ARCHITECTURE.md), [ADR 0019](../adr/0019-veach-modules-and-services.md) (modules and services).

## (a) A C++ component

```cpp
struct Health {
    float value = 100.0f;
    std::string label = "hp";
};

world.component<Health>()
    .member(&Health::value, "value")     // every field, member pointer first
    .member(&Health::label, "label")
    .add<veach::Scriptable>();           // optional: expose to Luau as veach.components.Health
```

- Reflection is mandatory: init fails with `ecs.unreflected_component` if any data member is missing.
- `Scriptable` components need Luau-representable fields: `bool`, integers, `float`/`double`,
  `std::string`, `veach::math::vec3`, reflected enums, or nested reflected structs of those.
- Engine components that scripts can see change `types/veach.d.luau`: regenerate it
  (`build/debug/tools/veach-cli/veach types --out types/veach.d.luau`) and commit it.
- Components are data only. Never put GPU handles or renderer data in components ([ADR 0020](../adr/0020-renderer-data-and-extraction.md)).

## (b) A system

```cpp
world.system<Health>("Regenerate")
    .kind<veach::phase::Simulation>()
    .each([](veach::Iter& it, std::size_t, Health& h) {
        h.value += 1.0f * it.delta_time();   // delta_time() is the fixed step, 1/60
    });
```

Pick the phase from [ecs-and-phases](../specs/ecs-and-phases/spec.md). Gameplay goes in
`Simulation`/`PostSimulation`; nothing in simulation reads wall-clock time or `std::rand`
(use the `veach::Random` singleton). Systems that create **named** entities need
`.immediate()` (see [flecs-v4/README.md](flecs-v4/README.md)).

## (c) A module

```cpp
struct GameModule {
    explicit GameModule(veach::World& world) {
        world.module<GameModule>();
        // components, systems, observers ...
    }
};

int main(int argc, char** argv) {
    veach::Engine engine({.name = "my-game", .default_project = "path/to/project"}, argc, argv);
    engine.import<GameModule>();
    return engine.run();
}
```

Modules are imported in call order during the `modules` init step, after every engine
subsystem exists. Keep module structs empty. Engine services (assets, scripts, renderer, runtime meshes,
platform, RHI device) are reachable with `veach::services(world).get<T>()` — for example
`veach::services(world).get<veach::AssetServer>()`. See `apps/lab/src/lab_module.cpp`.

## (d) A Luau binding

All in `engine/script/src/script_runtime.cpp` and `engine/script/src/luau_types.cpp`:

1. Add a static C function to `ScriptRuntime::Impl` (`static int api_thing(lua_State* S)`).
   Get the runtime with `Impl& rt = self(S);`. Push results on `S` (the calling thread), not `L`.
   Raise errors with `raise(S, ErrorCode::..., message, hint)`.
   - Writes go through `rt.require_active(S, "veach.thing()")` (the invocation's command buffer), never directly to the world.
   - Functions that change the world's shape (like `component`, `system`) call `rt.require_loading(S, ...)`.
   - Never add per-entity calls from C++ into Luau; pass batches.
2. Register it in `Impl::open()` on the `veach` table (or a sub-table) with
   `push_function(api_thing, "thing"); lua_setfield(L, -2, "thing");`.
3. Add the top-level name to `api_function_names`.
4. Add its type to `api_prelude` and/or the `Veach` type in `luau_types.cpp`.
   The test `[script.AC8]` fails if a name in `api_function_names` has no `read <name>:` type.
5. Regenerate `types/veach.d.luau`, add a test in `tests/script_tests.cpp`, and update
   [luau-api.md](luau-api.md) and the [scripting spec](../specs/scripting-luau-api-v0/spec.md) in the same commit.

## (e) A new engine subsystem (init step)

1. Create `engine/<area>/` with `include/veach/<area>/` and `src/`, and a `CMakeLists.txt` using
   `veach_add_engine_library(<area> SOURCES ... PUBLIC_DEPS ...)`. Depend only on lower layers
   (see the layering comment in `engine/CMakeLists.txt`); add `add_subdirectory(<area>)` there
   and link it from `engine/runtime/CMakeLists.txt`. Export headers from `engine/include/veach/veach.hpp` if users need them.
2. In `Engine::init()` (`engine/runtime/src/engine.cpp`), add a `run_step("<area>", init, teardown)`
   at the right place in the order. `init` returns a short detail string or an `Error`;
   `teardown` undoes exactly what `init` did. Make the object available with
   `services_.provide<T>(*obj)` and `services_.revoke<T>()` in the teardown.
3. Update the expected list in `tests/lifecycle_tests.cpp` (`[lifecycle.AC1]`, `[lifecycle.AC2]`),
   the init order in [ARCHITECTURE.md](../ARCHITECTURE.md), [ADR 0018](../adr/0018-engine-lifecycle-and-phases.md) and the
   [engine-lifecycle spec](../specs/engine-lifecycle/spec.md). Changing the order is an architecture change: write an ADR.

## (f) An asset loader

```cpp
struct TextAsset { std::string text; };

class TextLoader final : public veach::TypedAssetLoader<TextAsset> {
public:
    std::string_view name() const override { return "text"; }               // written to .meta
    std::span<const std::string_view> extensions() const override { return exts_; }
    veach::Result<TextAsset> load(const veach::LoadContext& ctx) const override {
        return TextAsset{std::string(ctx.bytes)};
    }
private:
    static constexpr std::array<std::string_view, 1> exts_{".txt"};
};

auto& assets = veach::services(world).get<veach::AssetServer>();
assets.add_loader(std::make_unique<TextLoader>());
veach::Handle<TextAsset> h = assets.load<TextAsset>("data/hello.txt");
```

Loaders must not touch the world (they will move to worker threads). Return an `Error` with
a code and hint on failure. See `engine/script/src/script_asset.cpp` for the real Luau loader
and the [asset spec](../specs/asset-system-core/spec.md).

## (g) An error code

1. Add `VEACH_ERROR(enumerator, "area.slug", "One-line summary")` to
   `engine/core/include/veach/core/error_codes.inl`. Never rename or reuse a slug.
2. Add a `## area.slug` section to [../reference/errors.md](../reference/errors.md) saying what it means and how to fix it.
   `tests/core_tests.cpp` fails if the section is missing.
3. Use it: `return veach::make_error(veach::ErrorCode::enumerator, "what happened, with values", "how to fix it");`

## (h) An extractor

Extraction is the only path from the ECS into the renderer ([ADR 0020](../adr/0020-renderer-data-and-extraction.md)). Register an
extractor instead of reading the world from renderer code:

```cpp
auto& renderer = veach::services(world).get<veach::render::Renderer>();
renderer.add_extractor("health_bars", [](const veach::World& w, veach::render::RenderWorld& out) {
    w.each([&](const Health&) { /* copy what the renderer needs into out */ });
});
```

Extractors run in registration order during the `Extract` phase, into a freshly cleared
`RenderWorld`. The engine's own extractors fill `views`, `meshes`, `lights` and `sky`.

## (i) A material

A Slang type conforming to `IMaterial` that overrides only the functions it changes, registered in the app's
CMake with `veach_add_material()`. See [rendering](rendering.md)
and [ADR 0035](../adr/0035-material-model.md).

## (j) Parallel work (jobs)

Never start a thread: get the engine's job system and give it the work ([spec](../specs/job-system/spec.md),
[ADR 0073](../adr/0073-job-system-enkits.md)).

```cpp
veach::JobSystem& jobs = veach::services(world).get<veach::JobSystem>();

// Outputs in indexed slots; the grain is part of the algorithm, never derived from the thread count.
std::vector<Mesh> meshes(chunks.size());
jobs.parallel_for("mesh chunks", chunks.size(), 1, [&](std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) meshes[i] = mesh(chunks[i]);
});

// Reductions combine chunk results in chunk order, so a float sum is the same on 1 or 18 threads.
const double total = jobs.parallel_reduce("total mass", bodies.size(), 1024, 0.0,
    [&](std::size_t begin, std::size_t end) { double m = 0; for (auto i = begin; i < end; ++i) m += bodies[i].mass; return m; },
    [](double a, double b) { return a + b; });
```

1. Prepare the inputs on the main thread, run the jobs, apply the outputs to the world on the main thread. A job
   never reads or writes the World or a `CommandBuffer`.
2. Pick a grain that gives each chunk at least ~10 µs of work and several chunks per thread.
3. Label every call: the label names the Tracy zone (`jobs` / `job`) on whichever worker runs it.
4. Test with `--jobs 1` and the default: canonical results must match bit for bit (see `tests/jobs_tests.cpp`).

## (k) An MCP tool

A module adds its own tools to `--mcp` through the `veach::McpTools` service, provided from the `ecs` init step on
([spec](../specs/mcp-server/spec.md), VCH-100). See `space/src/mcp_tools.cpp` (`orrery_state`, `generate_system`).

```cpp
#include <veach/runtime/mcp_tools.hpp>

auto added = veach::services(world).get<veach::McpTools>().add({
    .name = "health_of",                                  // unique; snake_case, prefixed with the module's area
    .description = "The Health of every entity that has one.",
    .input_schema = {{"type", "object"}, {"properties", {{"limit", {{"type", "integer"}, {"minimum", 1}}}}}},
    .handler = [](veach::World& w, const nlohmann::json& args) -> veach::Result<veach::McpToolOutput> {
        if (args.contains("limit") && !args["limit"].is_number_unsigned()) {
            return veach::make_error(veach::ErrorCode::mcp_invalid_arguments, "'limit' must be a positive integer");
        }
        nlohmann::json out = nlohmann::json::array();
        w.each([&](veach::Entity e, const Health& h) { out.push_back({{"entity", e.id()}, {"value", h.value}}); });
        return nlohmann::json{{"health", out}};           // sent as indented JSON text, like the built-in tools
    },
});
if (!added) {
    veach::log::error("game", added.error());             // mcp.duplicate_tool / mcp.invalid_tool, with a hint
}
```

1. Register in the module's constructor. A world made without an Engine (unit tests) may have no `McpTools`:
   use `veach::find_services(world)` and `find<veach::McpTools>()` if the module must work there too.
2. The handler runs on the main thread between frames, like the built-in tools; it may read the world and call
   engine services. Return `veach::McpContent{blocks}` instead of JSON for images or several content blocks.
3. Errors are `Error{code, message, hint}`: they reach the client as `isError` results with the `[code]`. Use
   `mcp.invalid_arguments` with a `did_you_mean` hint for bad names.
4. 64-bit integers are decimal strings in arguments and results ([ADR 0046](../adr/0046-64-bit-values-across-luau-json-mcp.md)).
5. Module tools are not in `--record` recordings: keep them read-only, or write through components an agent can see.
6. Test the handler with `McpTools::call` (see `tests/mcp_tools_tests.cpp`), and the app over stdio in
   `tests/mcp/test_mcp_lab.py`.

## (l) Procedural generation: a stage, a field graph or a generator hook

Generated content that gameplay reads goes through `engine/procgen`
([guide](procgen.md), [spec](../specs/procgen/spec.md)): records are C++ **stages** registered on a
`veach::procgen::StageRegistry` and read through a `StageRuntime`; per-sample functions are **field graphs** (JSON or
the Luau builder) compiled per body and evaluated on lattice points; small per-body programs are Luau **generator
hooks** in `veach::procgen::Sandbox`. See `space/gen/src/stages.cpp` for the toolbox's stages.

1. Stages and nodes are pure: no World, no command buffer, no scale factor, no clock, no `rand()`; randomness from
   the stage context's streams or the node's seed.
2. Canonical code only (ADR 0043): `veach::dmath` for transcendentals, `veach::hash` for randomness, no iteration
   over unordered containers where order reaches the output.
3. Commit a hash of the output in a unit test; a changed output is a new version, never an edited hash.
4. A field graph takes positions, never a level of detail ([ADR 0074](../adr/0074-amendments-from-sp-08.md)).
5. Nothing under `engine/procgen` links rhi or render; a GPU evaluator is a render feature (`check_repo.py`).
