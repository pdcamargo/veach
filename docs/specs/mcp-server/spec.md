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
| `screenshot` | `path?` (also write the PNG there) | image content (PNG) and a text note |
| `render_graph_plan` | | the last compiled plan as text (`RenderGraph::describe`) |
| `gpu_timings` | | `{backend, device, frames_in_flight, last_frame_gpu_ms}` |
| `eval` | `source` (Luau) | `{ok, returns: [string], log: [string]}` |

```cpp
// engine/runtime/include/veach/runtime/mcp_server.hpp
class McpServer {
public:
    explicit McpServer(Engine& engine);
    int run(std::istream& in, std::ostream& out);      // until EOF; Engine::run() calls this under --mcp
    nlohmann::json handle(const nlohmann::json& message);
};
```

## Acceptance criteria

All of them are checked by `tests/mcp/test_mcp_lab.py`, which drives `veach-lab` purely through MCP. CTest:
`mcp: tools over stdio (null RHI)` (AC1–AC8, AC10) and `mcp: golden through MCP (GPU)` (AC9, AC10).

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

## Out of scope

- Other transports (streamable HTTP, so a running editor can be shared by several clients): VCH-048.
- Per-pass GPU timings (`gpu_timings` reports the whole frame only): VCH-049.
- Resources and prompts, notifications on world changes, asset tools (import, reload).
- Authentication: stdio means the client spawned the process and already owns it.

## Open questions

None.

## Links

- ADRs: [0006 Luau scripting](../../adr/0006-scripting-luau.md), [0029 frame and sync model](../../adr/0029-frame-and-sync-model.md)
- Backlog: VCH-003, VCH-048, VCH-049
- Guides: [AGENTS.md](../../../AGENTS.md#driving-the-engine-over-mcp)
