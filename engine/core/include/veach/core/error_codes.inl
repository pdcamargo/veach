// Stable error codes. Each code MUST have a matching "## <slug>" section in
// docs/reference/errors.md (enforced by tests/core/test_error.cpp).
// Never rename or reuse a slug: tools and docs link to it.
//
// VEACH_ERROR(enumerator, "slug", "one-line summary")

VEACH_ERROR(internal, "core.internal", "An engine invariant was violated")
VEACH_ERROR(io_failed, "core.io_failed", "A file could not be read or written")

VEACH_ERROR(cli_unknown_flag, "cli.unknown_flag", "An unknown command-line flag was passed")
VEACH_ERROR(cli_invalid_value, "cli.invalid_value", "A command-line flag received an invalid value")
VEACH_ERROR(cli_conflicting_flags, "cli.conflicting_flags", "Two command-line flags contradict each other")

VEACH_ERROR(project_not_found, "project.not_found", "The project directory or veach.project.json was not found")
VEACH_ERROR(data_parse_failed, "data.parse_failed", "A JSON data file is not valid JSON")
VEACH_ERROR(data_unknown_field, "data.unknown_field", "A JSON data file contains a field the type does not have")
VEACH_ERROR(data_type_mismatch, "data.type_mismatch", "A JSON value has the wrong type for its field")

VEACH_ERROR(engine_init_failed, "engine.init_failed", "An engine initialization step failed")

VEACH_ERROR(ecs_unreflected_component, "ecs.unreflected_component",
            "A component was registered without reflection data")
VEACH_ERROR(ecs_unknown_component, "ecs.unknown_component", "A component name does not name a reflected component")
VEACH_ERROR(ecs_unknown_entity, "ecs.unknown_entity", "An entity id or path does not name a live entity")
VEACH_ERROR(mcp_unknown_tool, "mcp.unknown_tool", "An MCP client called a tool that does not exist")
VEACH_ERROR(mcp_invalid_arguments, "mcp.invalid_arguments", "An MCP tool call has missing or ill-typed arguments")
VEACH_ERROR(ecs_unknown_phase, "ecs.unknown_phase", "A system was attached to a phase that does not exist")

VEACH_ERROR(platform_init_failed, "platform.init_failed", "The platform backend failed to initialize")
VEACH_ERROR(platform_window_failed, "platform.window_failed", "The OS window could not be created")
VEACH_ERROR(input_unknown_action, "input.unknown_action", "A script or tool named an input action that is not bound")
VEACH_ERROR(input_unknown_key, "input.unknown_key",
            "An input binding or injected event names a key that does not exist")

VEACH_ERROR(asset_not_found, "asset.not_found", "An asset path does not exist")
VEACH_ERROR(asset_no_loader, "asset.no_loader", "No loader is registered for an asset's file extension")
VEACH_ERROR(asset_load_failed, "asset.load_failed", "A loader failed to load an asset")
VEACH_ERROR(asset_meta_invalid, "asset.meta_invalid", "An asset's .meta sidecar is malformed")

VEACH_ERROR(script_compile_failed, "script.compile_failed", "A Luau script has a syntax error")
VEACH_ERROR(script_runtime_error, "script.runtime_error", "A Luau script raised an error; its writes were discarded")
VEACH_ERROR(script_unknown_component, "script.unknown_component", "A script used a component name that does not exist")
VEACH_ERROR(script_duplicate_system, "script.duplicate_system", "Two scripts registered a system with the same name")
VEACH_ERROR(script_load_time_only, "script.load_time_only", "A load-time-only function was called from inside a system")
VEACH_ERROR(script_unknown_module, "script.unknown_module", "require() was called with an unknown module")
VEACH_ERROR(script_component_conflict, "script.component_conflict",
            "A script redefined a component with different fields")
VEACH_ERROR(script_invalid_value, "script.invalid_value", "A Luau value does not match the component's field type")
VEACH_ERROR(script_unsupported_field, "script.unsupported_field",
            "A component field type cannot be represented in Luau")
VEACH_ERROR(procgen_budget_exceeded, "procgen.budget_exceeded",
            "A generator hook ran past its safepoint, wall-clock or memory budget")

VEACH_ERROR(rhi_backend_unavailable, "rhi.backend_unavailable",
            "The requested RHI backend is not available on this platform")
VEACH_ERROR(rhi_not_implemented, "rhi.not_implemented", "The requested RHI feature is not available on this backend")
VEACH_ERROR(rhi_gpu_error, "rhi.gpu_error", "The GPU reported an error while executing commands")
VEACH_ERROR(render_screenshot_failed, "render.screenshot_failed", "A screenshot could not be captured or written")
VEACH_ERROR(render_graph_invalid, "render.graph_invalid", "A render graph pass declared an impossible resource access")
VEACH_ERROR(replay_invalid, "replay.invalid", "A recording cannot be replayed with this run")
VEACH_ERROR(replay_diverged, "replay.diverged", "A replay's simulation state differs from the recording's")
VEACH_ERROR(render_history_lost, "render.history_lost",
            "Path-traced pixels read back something other than the accumulation they last wrote")
VEACH_ERROR(render_nonfinite_samples, "render.nonfinite_samples", "The path tracer produced NaN or infinite samples")
VEACH_ERROR(render_mesh_invalid, "render.mesh_invalid", "A runtime mesh submitted to the MeshStore is malformed")
VEACH_ERROR(render_mesh_unknown, "render.mesh_unknown", "A MeshStore call named a mesh or material it does not hold")
VEACH_ERROR(rhi_invalid_surface, "rhi.invalid_surface", "The native window handle given to the RHI is invalid")
VEACH_ERROR(rhi_invalid_descriptor, "rhi.invalid_descriptor",
            "A shader used a descriptor handle that names no live resource of its kind")
VEACH_ERROR(rhi_invalid_texture, "rhi.invalid_texture", "A texture description cannot be created on every backend")
VEACH_ERROR(rhi_invalid_pipeline, "rhi.invalid_pipeline", "A pipeline description is inconsistent")
