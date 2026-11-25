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
`backspace`, the arrows `left`/`right`/`up`/`down`, `left_shift`, `right_ctrl`…, `f1`–`f12`, and the mouse
buttons `mouse_left`, `mouse_right`, `mouse_middle`. The hint suggests the closest one.

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

## rhi.backend_unavailable

**The requested RHI backend is not available on this platform.**

The backend does not exist on this OS (Metal is macOS only, DX12 Windows only) or no GPU was found. `null` works everywhere.

## rhi.not_implemented

**The requested RHI feature is not available on this backend.**

For example, a shader library without code for this backend (no DXIL or metallib for an entry point), or a format
the backend can't use for a texture. The message names what is missing and the hint says how to provide it.

## rhi.invalid_surface

**The native window handle given to the RHI is invalid.**

Surfaces need a window from the SDL platform backend. Headless runs have no native window, so the engine does not create a surface for them.

## rhi.invalid_descriptor

**A shader used a descriptor handle that names no live resource of its kind.**

Debug builds check every `DescriptorHandle` a shader dereferences (ADR 0039): a handle never set (0), past the
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

## rhi.gpu_error

**The GPU reported an error while executing commands.**

Logged when a submission completes with an error, for example a page fault, a timeout or a validation failure in
an encoder. The message names the submission. Run a debug build, where Metal API validation is on, to get the
API call that caused it. The run exits with code 1.

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

The path tracer keeps a running mean per pixel and stores the sample count next to it. Every frame checks
that each pixel reads back the count it wrote, and that the mean is finite and not negative. A mismatch means
the accumulation memory was lost or overwritten between frames (a synchronization, residency or aliasing bug),
which shows as dark blocks that fade slowly. The message gives the number of pixels and the frame. Rerun with
`MTL_SHADER_VALIDATION=1` (Metal) or the D3D12 debug layer, or take a GPU capture of that frame.

## render.nonfinite_samples

**The path tracer produced NaN or infinite samples.**

They are left out of the image, so one bad sample cannot blacken a pixel for good, but they are counted and
logged with the frame: a NaN means a bug in a material, a light or the path tracer (a division by a zero pdf,
the normal of a degenerate triangle, a NaN light intensity). The run exits with code 1.

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
