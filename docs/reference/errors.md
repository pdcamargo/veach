# Error reference

Every Veach error has a stable code (the `[slug]` at the end of the message, or the `code` field in JSON logs). Codes never change meaning and are never reused. Each section says what the error means and how to fix it.

The list is defined in `engine/core/include/veach/core/error_codes.inl`. `tests/core_tests.cpp` fails if a code has no section here.

## core.internal

**An engine invariant was violated.**

An engine bug, not a usage error. The message names the failed invariant and source location. Report it with the log (run with `--log-format json` to attach machine-readable output).

## core.io_failed

**A file could not be read or written.**

Check that the path exists and is readable or writable. Relative paths are resolved against the project directory, not the working directory.

## cli.unknown_flag

**An unknown command-line flag was passed.**

The hint suggests the closest known flag. `--help` lists every core flag. Flags are the same for every Veach app.

## cli.invalid_value

**A command-line flag received an invalid value.**

The message shows the value received; the hint lists accepted values. Numbers must be non-negative integers.

## cli.conflicting_flags

**Two command-line flags contradict each other.**

Pass only one of the conflicting flags (for example `--headless` or `--window`, not both).

## project.not_found

**The project directory or veach.project.json was not found.**

`--project` must point at a directory containing `veach.project.json`, or at the file itself.

## data.parse_failed

**A JSON data file is not valid JSON.**

The file is not valid JSON. The message includes the line and column. JSON allows no comments and no trailing commas.

## data.unknown_field

**A JSON data file contains a field the type does not have.**

The field does not exist on the reflected type. The hint suggests the closest field. The generated schema in `schemas/` lists every field; editors that understand `$schema` autocomplete them.

## data.type_mismatch

**A JSON value has the wrong type for its field.**

The value has the wrong JSON type, or an enum value is not one of the constants (the hint lists them).

## engine.init_failed

**An engine initialization step failed.**

An init step failed; the message names the step and the underlying error. Steps that had completed were shut down in reverse order.

## ecs.unreflected_component

**A component was registered without reflection data.**

Add reflection for every listed component: `world.component<T>().member(&T::field, "field")` for each field. Tags (empty structs) need none. Reflection is mandatory: tools, scripts, schemas and MCP cannot see unreflected data.

## ecs.unknown_phase

**A system was attached to a phase that does not exist.**

Phases are fixed: Startup, Input, Simulation, PostSimulation, TransformPropagation, PreExtract, Extract, Render, Shutdown. The hint suggests the closest one.

## platform.init_failed

**The platform backend failed to initialize.**

SDL could not start (usually: no display). Run with `--headless` in CI, over SSH or in containers.

## platform.window_failed

**The OS window could not be created.**

The OS refused to create the window. Check the size in `veach.project.json`, or run `--headless`.

## input.unknown_action

**A script or tool named an input action that is not bound.**

Actions come from `"input"` in `veach.project.json` (or `veach::bind_action` in C++). The hint suggests the
closest bound action. Add the action to the project file, or fix the name. See
[input](../specs/input/spec.md).

## input.unknown_key

**An input binding or injected event names a key that does not exist.**

Key names are the constants of `veach::Key`: `a`–`z`, `digit0`–`digit9`, `space`, `enter`, `escape`, `tab`,
`backspace`, the arrows `left`/`right`/`up`/`down`, `left_shift`, `right_ctrl`…, `f1`–`f12`, the mouse
buttons `mouse_left`, `mouse_right`, `mouse_middle`, and the gamepad buttons `gamepad_south`, `gamepad_east`,
`gamepad_west`, `gamepad_north`, `gamepad_left_shoulder`, `gamepad_dpad_up`…. The hint suggests the closest one.

## input.unknown_axis

**An analog axis name in a project or tool is not a `veach::Axis`.**

The `source` of an entry under `"axes"` in `veach.project.json`, or the `axis` of MCP `send_input`, must be one
of `mouse_x`, `mouse_y`, `mouse_wheel_x`, `mouse_wheel_y`, `gamepad_left_x`, `gamepad_left_y`,
`gamepad_right_x`, `gamepad_right_y`, `gamepad_left_trigger`, `gamepad_right_trigger`. The hint suggests the
closest one. See [input](../specs/input/spec.md).

## asset.not_found

**An asset path does not exist.**

The path is relative to the project directory. The hint suggests a similarly named file in the same folder.

## asset.no_loader

**No loader is registered for an asset's file extension.**

No registered loader claims the file extension for the requested asset type. Register one with `AssetServer::add_loader()`.

## asset.load_failed

**A loader failed to load an asset.**

The loader rejected the file; the message carries the loader's reason.

## asset.meta_invalid

**An asset's .meta sidecar is malformed.**

The `.meta` sidecar is not valid JSON or has a malformed `id`. Restore it from version control. Deleting it mints a new id, which breaks references to the asset.

## script.compile_failed

**A Luau script has a syntax error.**

Luau syntax error. The message has `file:line`. Run `luau-analyze` on the file for full diagnostics.

## script.runtime_error

**A Luau script raised an error; its writes were discarded.**

A script raised an error. For a script's top level, nothing it did was applied: no components written, no systems registered. For a system, that run's writes were discarded and the system runs again next time. The process exits with code 1 at the end of the run.

## script.unknown_component

**A script used a component name that does not exist.**

The component name is not defined. Define it with `veach.component(name, defaults)` or use a C++ component from `veach.components`. The hint suggests the closest name.

## script.duplicate_system

**Two scripts registered a system with the same name.**

System names are global across scripts. Rename one of them.

## script.load_time_only

**A load-time-only function was called from inside a system.**

`veach.component` and `veach.system` can only be called while a script's top level runs, not from inside a system.

## script.unknown_module

**require() was called with an unknown module.**

Only `require("@veach")` exists in v0.

## script.component_conflict

**A script redefined a component with different fields.**

A script component with this name already exists with different fields, or the name is taken by a C++ component or entity. Use a different name or make the definitions identical.

## script.invalid_value

**A Luau value does not match the component's field type.**

A value written from Luau does not match the field's type, or names a field that does not exist (the hint suggests the closest one). Field types: boolean, number, string, vector, or an enum constant name.

## script.unsupported_field

**A component field type cannot be represented in Luau.**

The field type cannot be represented in Luau (arrays, maps, opaque types other than strings). Script component fields may be boolean, number, string or vector.

## procgen.budget_exceeded

**A generator hook ran past its safepoint, wall-clock or memory budget.**

A Luau generator hook in the procgen sandbox ([ADR 0052](../adr/0052-procgen-runtime.md)) was stopped and its outputs
were discarded. The message says which budget: **safepoints** (loop iterations, calls and returns; deterministic, so
the same hook fails the same way on every machine), **wall clock** (a hang guard that differs between machines) or
**memory**. Usually a loop never ends or a table grows without end. If the work is genuinely that large, split it
into several calls (per cube face or per row) or raise the budget in `SandboxOptions`.

## procgen.graph_invalid

**A field graph is malformed: an unknown node type, a bad port, a length without a unit class, a cycle.**

A field graph ([procgen spec](../specs/procgen/spec.md), [guide](../guides/procgen.md)) failed to load or compile. The
message names the graph and the node; the hint says the fix. Common causes: a node type without its version
(`veach:fbm` instead of `veach:fbm@1`) or a version this build does not have; a length written as a bare number
(write `{"planet_m": 3000000}` or `{"m": 3}`); a length whose unit class does not match the position it samples
(`planet_m` lengths go with `p_planet`, `m` lengths with `p_local`); a port wired to the wrong kind of value (a
position input into a scalar port); a cycle. Unknown fields and parameters are `data.unknown_field`, invalid JSON
`data.parse_failed`.

## procgen.stage_invalid

**A generation stage cannot be registered: a duplicate id and version, a malformed id, a tier violation.**

`StageRegistry::add` refused a stage ([ADR 0051](../adr/0051-staged-generation-pipeline.md)). Stage ids are
`namespace:name` (lowercase, digits, `_`, `.` and `-`); each (id, version) is registered once, so a changed output
needs a new version beside the old one ([ADR 0055](../adr/0055-universe-identity-and-versioning.md)). Data flows only
down the determinism tiers ([ADR 0043](../adr/0043-determinism-tiers-and-strict-floating-point.md)): a canonical stage
cannot read a replay or visual one.

## procgen.stage_failed

**A generation stage failed; nothing of its output was kept.**

A stage function returned an error, or one of its inputs failed. The message names the stage, its version and the
address, followed by the original error, whose hint still applies. Nothing is cached, so fixing the input or the
stage and asking again re-runs it.

## rhi.backend_unavailable

**The requested RHI backend is not available on this platform.**

The backend does not exist on this OS (Metal is macOS only, DX12 Windows only), no GPU meets the feature floor
([ADR 0003](../adr/0003-platforms-and-hardware-target.md)), or a feature needs a capability the device lacks: photo
mode needs hardware ray tracing (`VEACH_DISABLE_RAY_TRACING=1` turns it off on purpose). `null` works everywhere.
Golden tests treat this error as a skip, unless `VEACH_REQUIRE_GPU=1`.

## rhi.not_implemented

**The requested RHI feature is not available on this backend.**

For example, a shader library without code for this backend (no DXIL or metallib for an entry point), or a format
the backend can't use for a texture. The message names what is missing and the hint says how to provide it.

## rhi.invalid_surface

**The native window handle given to the RHI is invalid.**

Surfaces need a window from the SDL platform backend. Headless runs have no native window, so the engine does not create a surface for them.

## rhi.invalid_descriptor

**A shader used a descriptor handle that names no live resource of its kind.**

Debug builds check every `DescriptorHandle` a shader dereferences (ADR 0027): a handle never set (0), past the
end of the heap, of the wrong kind (a buffer handle used as a texture), or kept after its resource was destroyed.
The shader goes on with the kind's null descriptor, and the message names the first bad handle and the frame.
Set every handle a shader can read, including in records it may only read behind a flag, and use
`device.null_descriptor(kind)` for "no resource". DX12 checks in the shader; on Metal, Metal's shader validation
(on in debug builds) reports the same mistakes as `rhi.gpu_error`.

## rhi.invalid_texture

**A texture description cannot be created on every backend.**

Block-compressed textures (BC7) need a width and height that are multiples of 4: D3D12 cannot copy into an
unaligned top level. The texture cooker resizes images to meet this, so the error means a texture made in code.
Round its size up to a multiple of 4.

## rhi.invalid_pipeline

**A pipeline description is inconsistent.**

A graphics pipeline that tests or writes depth needs `depth_format` (the render pass's depth attachment format,
`depth32_float`), and `depth_format` must be a depth format. Every backend checks this before creating the
pipeline, so a pipeline that works on one works on all.

## rhi.gpu_error

**The GPU reported an error while executing commands.**

Logged when a submission completes with an error, for example a page fault, a timeout or a validation failure in
an encoder. The message names the submission. Run a debug build, where Metal API validation is on, to get the
API call that caused it. The run exits with code 1.

## replay.invalid

**A recording cannot be replayed with this run.**

The file is missing, malformed or from another recording format, or the run asks for something the recording
contradicts: a different `--script` or `--seed` (a replay takes both from the recording; leave them out). Record
again with `--record` (docs/specs/record-replay/spec.md).

## replay.diverged

**A replay's simulation state differs from the recording's.**

Every frame of a recording carries a hash of the simulation state (every entity's Scriptable components); the
replay stops at the first frame whose state differs and names the component. Something in simulation is not a
function of the seed, the script and the recorded input: wall-clock time, an unseeded random number, iteration
order of an unordered container, or world edits made over MCP while recording (not recorded yet).

## render.screenshot_failed

**A screenshot could not be captured or written.**

`--screenshot` needs a GPU backend (`--rhi metal`; the null backend draws nothing) and `--frames N`, because it
captures the last frame. The message says which of these was missing, or why the PNG could not be written.

## render.graph_invalid

**A render graph pass declared an impossible resource access.**

For example, a pass reads a transient texture that no earlier pass writes. The message names the pass and the
resource. `RenderGraph::describe()` (logged at debug level) shows the compiled order and barriers.

## render.history_lost

**Path-traced pixels read back something other than the accumulation they last wrote.**

Photo mode's path tracer keeps a running mean per pixel and stores the sample count next to it. Every frame checks
that each pixel reads back the count it wrote, and that the mean is finite and not negative. A mismatch means
the accumulation memory was lost or overwritten between frames (a synchronization, residency or aliasing bug),
which shows as dark blocks that fade slowly. The message gives the number of pixels and the frame. Rerun with
`MTL_SHADER_VALIDATION=1` (Metal) or the D3D12 debug layer, or take a GPU capture of that frame.

## render.nonfinite_samples

**The path tracer produced NaN or infinite samples.**

Photo mode leaves them out of the image, so one bad sample cannot blacken a pixel for good, but they are counted and
logged with the frame: a NaN means a bug in a material, a light or the path tracer (a division by a zero pdf,
the normal of a degenerate triangle, a NaN light intensity). The run exits with code 1.

## render.nonfinite_pixels

**The real-time renderer wrote NaN or infinite radiance.**

With auto-exposure on (spike SP-14, [findings](../research/spikes/sp-14-exposure-depth.md)), the exposure pass checks
every pixel of the pre-exposed HDR target and logs the count with the frame. Infinity means a radiance above the fp16
maximum (65,504) reached the target unclamped, such as the sun's disk at a dark exposure; NaN means a bug in a material,
a light or the sky (a division by zero, a degenerate normal). The run exits with code 1. Reproduce headless with the
same frame count and look at the frame named.

## render.mesh_invalid

**A runtime mesh submitted to the MeshStore is malformed.**

`MeshStore::add()`, `update()` and `build_runtime_mesh()` check what a module submits
([runtime meshes](../specs/runtime-meshes/spec.md)): vertices and whole triangles, every index inside the vertex
array, finite positions, one slot per triangle (or none) and at most 256 material slots. The message names the first
problem and its values. Nothing is stored; an update keeps drawing the previous data.

## render.mesh_unknown

**A MeshStore call named a mesh or material it does not hold.**

`update()`, `remove()`, `update_material()` and `remove_material()` take the id that `add()` or `add_material()`
returned. The id was never made by this store, or was already removed. Ids are never reused.

## ecs.unknown_component

**A component name does not name a reflected component.**

Used by tools (MCP `get_component`, `set_component`, `query_entities`). Names are the component's short name
(`Window`, `TriangleDemo`) or its full path (`veach::platform::Window`). The hint suggests the closest one;
`tools/list` shows them all in the schema's `enum`.

## ecs.unknown_entity

**An entity id or path does not name a live entity.**

Entities are addressed by numeric id (from `query_entities` or `spawn_entity`) or by path (`primary_window`,
`lab::spinner`). The entity may have been despawned.

## mcp.unknown_tool

**An MCP client called a tool that does not exist.**

The hint suggests the closest tool name; `tools/list` returns every tool with its schema.

## mcp.invalid_arguments

**An MCP tool call has missing or ill-typed arguments.**

The message names the argument. Each tool's `inputSchema` (from `tools/list`) describes the expected arguments.

## mcp.duplicate_tool

**A module registered an MCP tool name that is already taken.**

`McpTools::add` refuses a name that a built-in tool (`eval`, `step_frames`...) or another module already uses. Rename
the tool, prefixed with the module's area (`physics_raycast`), or register it from one module only.

## mcp.invalid_tool

**A module registered an MCP tool with a bad name, schema or handler.**

Names are 1-64 characters of `A-Z a-z 0-9 _ - .` (Veach uses snake_case), the input schema is a JSON Schema with
`"type": "object"`, and the handler is set. Guide: [extending](../guides/extending.md#k-an-mcp-tool).
