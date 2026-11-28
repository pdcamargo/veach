# MCP server v0

**Status:** Implemented
**Area:** runtime
**Owner:** dev lead

## Summary

Any Veach app started with `--mcp` is a Model Context Protocol server on stdio. An agent can inspect and edit
the live world, step frames, take screenshots and run Luau through tools whose schemas come from reflection.
This is how Claude Code (or any MCP client) drives the engine without a human at the keyboard.

## Motivation

Veach is AI-native: agents are first-class users of the engine, not just of its source code. Until now an agent
could only build, run a script and read logs. With MCP it can ask what is in the world, change it, and check the
result as an image, which is the loop a human gets from an editor. Backlog: VCH-003.

## Requirements

- MUST speak MCP over stdio: JSON-RPC 2.0, one message per line, `initialize` (echoing a supported protocol
  version), `ping`, `tools/list`, `tools/call`. Unknown methods return `-32601`, unparseable lines `-32700`.
- In MCP mode stdout MUST carry only protocol messages. Logs go to stderr.
- Headless, frames MUST advance only through `step_frames`, one fixed 1/60 s step per frame, so a session is
  deterministic. Windowed, the app MUST keep rendering between requests.
- Tool input schemas MUST be generated from reflection: the component name enum, and each component's value
  schema (the same JSON Schema `--write-types` writes).
- Tool errors MUST be MCP tool results with `isError: true`, and their text MUST carry the Veach error code
  (`[ecs.unknown_component]`) and a did-you-mean hint when a name is close to a known one.
- Writes (`set_component`, `spawn_entity`, `eval`) MUST be all or nothing: validated first, applied through a
  command buffer, and discarded on error.
- `eval` MUST run in the same sandbox as scripts (`require("@veach")` only) and return its return values (as
  `tostring`) and the log lines it produced.
- `screenshot` MUST render the current state without advancing the simulation and return the PNG as MCP image
  content, headless or windowed. On the null RHI it fails with `render.screenshot_failed`.
- The server MUST exit 0 when the client closes stdin.
- Modules MUST be able to add tools (VCH-100) through the `McpTools` service, which the engine provides from the
  `ecs` init step, before any module is imported. A tool has a name, a description, a JSON Schema for its arguments
  (an object schema) and a handler `(World&, const json& arguments) -> Result<McpToolOutput>`.
- Tool names MUST be unique: registering a built-in or already registered name fails with `mcp.duplicate_tool`, and
  a bad name (not 1-64 of `A-Z a-z 0-9 _ - .`), a non-object schema or a missing handler with `mcp.invalid_tool`,
  each with a hint, and nothing is registered.
- `tools/list` MUST list module tools after the built-in ones, in registration order, and `initialize`'s instructions
  name them. Their handlers MUST run on the main thread between frames, like the built-in tools, and their errors
  MUST be tool results with `isError` and the error's code, like the built-in tools'. A misspelled tool name's
  did-you-mean covers module tools.

## API sketch

```sh
veach-lab --mcp --headless --rhi metal           # a deterministic session for an agent
veach-lab --mcp --rhi metal                      # windowed: the app keeps running, the agent pokes at it
```

| Tool | Arguments | Returns |
|---|---|---|
| `query_entities` | `with?: [component]`, `name_contains?`, `limit?` | `[{id, name, components}]` |
| `get_component` | `entity` (id or path), `component` | the value as JSON |
| `set_component` | `entity`, `component`, `value` (any subset of fields) | the new value |
| `spawn_entity` | `name?`, `components?: {Component: value}` | `{id, name, components}` |
| `step_frames` | `count?` (default 1) | `{frame, fixed_step, elapsed, running}` |
| `reload` | `path?` | `{reloaded: [{path, id, reloaded, error?}]}`: hot reload ([hot-reload](../hot-reload/spec.md)) |
| `list_assets` | | `{assets: [{path, id, loader, state, reloads, error?, reload_error?}]}` |
| `send_input` | `key` (a `veach::Key` name), `down`; or `axis` (a `veach::Axis` name), `value`; or `gamepad` (true/false) | `{key, down, delivered}`, `{axis, value, relative, delivered}` (the value as quantized) or `{gamepad, delivered}`; the event drives input actions and axes on the next frame ([input](../input/spec.md)) |
| `screenshot` | `path?` (also write the PNG there) | image content (PNG) and a text note |
| `photo_mode` | `samples`, `path?` | restarts photo mode (the reference path tracer, on the camera's `PhotoMode`) and renders exactly `samples` per pixel, without advancing simulation; image content (PNG) ([photo-mode](../photo-mode/spec.md)) |
| `render_graph_plan` | | the last compiled plan as text (`RenderGraph::describe`) |
| `gpu_timings` | | `{backend, device, gpu, frames_in_flight, last_frame_gpu_ms, frame, passes: [{name, gpu_ms}], passes_unavailable?, path_tracer_samples, culling}` (`path_tracer_samples`: photo mode's samples per pixel so far; `culling`: the real-time renderer's CullStats, [gpu-driven renderer](../gpu-driven-renderer/spec.md)): waits for the GPU, then the last completed frame's time per render-graph pass, in execution order; `passes_unavailable` says why `passes` is empty on a device that cannot time them (the null RHI). VCH-049 |
| `eval` | `source` (Luau) | `{ok, returns: [string], log: [string]}` |

Module tools of the lab, registered by the space toolbox's `SpaceMcpModule` (`space/src/mcp_tools.cpp`). 64-bit
values are decimal strings ([ADR 0046](../../adr/0046-64-bit-values-across-luau-json-mcp.md)). Both are read-only.

| Tool | Arguments | Returns |
|---|---|---|
| `orrery_state` | `system?` (`sol`, `earthlike`, `reference`), `scale?`, `tick?` (string), `body?`, `latitude?`, `longitude?` (degrees) | `{system, scale, tick, source, star, bodies: [{name, parent, model, position, velocity, distance_to_parent, distance_to_star, radius, orbit: {period_s, phase}, spin: {sidereal_period_s, phase, local_solar_time_at_prime_meridian, subsolar_point}}], sun?: {elevation, azimuth, local_solar_time, distance, direction_enu}}`: the SP-04 preset at a tick, computed on the spot. Defaults follow the world's `Orrery` and its `UniverseClock`, else Sol at J2000.0 |
| `generate_system` | `seed` (string), `address` (`"S:<16 hex>"`), `habitability_bias?` | `{address, seed, coords, cell_origin_ly, cell_edge_ly, stages: {"space:system": {version, hash}, "space:body.physics": {...}}, star, disk_*, snow_line_m, hz_*, planet_count, bodies: [{index, address, parent, composition, mass_kg, ..., orbit, physics}], violations: [string]}`: SP-05's S1 and B1 records |

```cpp
// engine/runtime/include/veach/runtime/mcp_server.hpp
class McpServer {
public:
    explicit McpServer(Engine& engine);
    int run(std::istream& in, std::ostream& out);      // until EOF; Engine::run() calls this under --mcp
    nlohmann::json handle(const nlohmann::json& message);
};

// engine/runtime/include/veach/runtime/mcp_tools.hpp: a service, services(world).get<McpTools>()
using McpToolOutput = std::variant<nlohmann::json, McpContent>;   // JSON (sent as text) or content blocks
struct McpTool {
    std::string name, description;
    nlohmann::json input_schema = {{"type", "object"}};
    std::function<Result<McpToolOutput>(World&, const nlohmann::json& arguments)> handler;
};
class McpTools {
public:
    Result<> add(McpTool tool);                 // mcp.duplicate_tool, mcp.invalid_tool
    bool remove(std::string_view name);
    const McpTool* find(std::string_view name) const;
    nlohmann::json list() const;                // tools/list entries
    Result<McpToolOutput> call(std::string_view name, World&, const nlohmann::json& arguments) const;
};
std::span<const std::string_view> builtin_mcp_tools();
```

## Acceptance criteria

AC1–AC10 are checked by `tests/mcp/test_mcp_lab.py`, which drives `veach-lab` purely through MCP. CTest:
`mcp: tools over stdio (null RHI)` (AC1–AC8, AC10, AC12–AC14) and `mcp: golden through MCP (GPU)` (AC9, AC10).
AC11–AC14 are also unit tests in `tests/mcp_tools_tests.cpp`.

- **mcp.AC1**: `initialize` negotiates the client's protocol version, and `tools/list` lists every v0 tool.
- **mcp.AC2**: The component enum lists engine, module and script-defined components, and `set_component`'s
  value schema for a component matches its reflected fields.
- **mcp.AC3**: `query_entities` finds the primary window, `get_component` reads it, and `set_component` changes
  the given fields and keeps the rest.
- **mcp.AC4**: A misspelled component name fails with `[ecs.unknown_component]` and `Did you mean 'Window'?`.
- **mcp.AC5**: `spawn_entity` creates an entity with a script-defined component.
- **mcp.AC6**: `step_frames` with `count: 3` advances exactly three frames and three fixed steps.
- **mcp.AC7**: `eval` returns its values and its log lines. A failing `eval` is an error and none of its writes
  are applied.
- **mcp.AC8**: `render_graph_plan` returns the plan, and `gpu_timings` reports the backend and frames in flight.
- **mcp.AC9**: Spawn a ray-traced `TriangleDemo`, step three frames and take a screenshot: the returned PNG
  matches `tests/golden/triangle_raytraced.png` (FLIP mean ≤ 0.01). Skipped without a capable GPU unless
  `VEACH_REQUIRE_GPU=1`.
- **mcp.AC10**: The server exits 0 when the client disconnects.
- **mcp.AC11**: `McpTools::add` refuses a built-in name, a name already registered, a bad name, a non-object schema
  and a missing handler with `mcp.duplicate_tool` or `mcp.invalid_tool` and a hint, and registers nothing; `call`
  of an unknown name fails with `mcp.unknown_tool` and a did-you-mean.
- **mcp.AC12**: A test module's tools are listed after exactly the built-in tools and named in `initialize`'s
  instructions; calling one runs its handler on the live world (it sees frames advanced by `step_frames`), its errors
  are `isError` results with the code, content blocks pass through, and a misspelled name suggests it.
- **mcp.AC13**: `orrery_state` gives, in one call, the Earth 0.98332 AU from the Sun at J2000.0 and the sun 18.09°
  over Paris at 12:06 local solar time (SP-04's checks), the Earth-like planet at noon on its prime meridian at
  tick 0, defaults from the world's orrery, and errors with did-you-mean hints for systems and bodies.
- **mcp.AC14**: `generate_system { seed: "42", address: "S:3f2a91c07e5b0000" }` returns SP-05's records with the stage
  hashes `"2733265849347982535"` (S1) and `"9754287468050259825"` (B1), equal to calling the stages directly.

## Out of scope

- Other transports (streamable HTTP, so a running editor can be shared by several clients): VCH-048.
- Resources and prompts, notifications on world changes, asset tools (import, reload).
- Authentication: stdio means the client spawned the process and already owns it.
- Recording module tool calls with `--record`: only the built-in edits are recorded, so module tools should be
  read-only (both of the lab's are).
- `notifications/tools/list_changed`: module tools are registered at init; `listChanged` stays false.

## Open questions

None.

## Links

- ADRs: [0007 Luau scripting](../../adr/0007-scripting-luau.md), [0030 frame and sync model](../../adr/0030-frame-and-sync-model.md), [0019 modules and services](../../adr/0019-veach-modules-and-services.md), [0046 64-bit values](../../adr/0046-64-bit-values-across-luau-json-mcp.md)
- Backlog: VCH-003, VCH-048, VCH-049, VCH-100
- Guides: [AGENTS.md](../../../AGENTS.md#driving-the-engine-over-mcp)
