# Hot reload

**Status:** Implemented
**Area:** asset
**Owner:** dev lead

## Summary

Edit a file and the running app picks it up. Assets reload in place behind the same id and handles. A script
that already ran runs again **as if fresh**: its systems are replaced by name, and the entities its top level
spawned are respawned. A reload that fails changes nothing and says why. Windowed runs watch files on their own;
headless runs reload only when asked (MCP `reload`), so they stay deterministic. MCP can trigger reloads and
observe them (`reload`, `list_assets`).

## Motivation

Iteration speed for people and agents: change a script, see the result without restarting. Backlog: VCH-004.

## Requirements

- `AssetServer::reload(path)` and `reload_changed()`. A file changed if its write time or size differs from
  when it was last read (`fs::stamp`, the platform layer's polling watcher). The new data replaces the old under
  the same `AssetId`. Pointers from `get()` are invalid after a reload; handles stay valid.
- A failed load keeps the previous data, logs at error level, and is reported in the `ReloadResult` and in
  `list()` as `reload_error`.
- Listeners (`on_reloaded`) run after the swap. The engine registers one for scripts.
- Script reload runs the new top level in a transaction, with the script's own system names free for reuse:
  - On success: delete the old version's systems and the entities its top level spawned (their children go
    with them), then apply the new writes and activate the new systems.
  - On failure: discard everything new, and the old version keeps running.
  - Entities spawned later, by systems, are kept.
  - Changing a script component's fields is a conflict (`script.component_conflict`); restart for that.
- `Engine::hot_reload(path?)` runs between frames. A windowed run calls it every 250 ms when the project's
  `hot_reload` is true (the default). A headless run never calls it by itself.
- MCP: `reload { path? }` returns `{ reloaded: [{ path, id, reloaded, error? }] }`, and an unknown path is an
  `asset.not_found` tool error with a suggestion. `list_assets` returns path, id, loader, state, reloads and the
  last errors.

## Acceptance criteria

- **reload.AC1**: A changed asset reloads behind the same handle and counts the reload. A broken version keeps
  the previous data and records `reload_error`. Reloading an unknown path suggests the closest loaded one.
- **reload.AC2**: A changed script's system runs its new code from the next frame, with the same number of
  systems. The entity its top level spawned is replaced by a fresh one with the same name, and an entity
  spawned by a system survives.
- **reload.AC3**: A script reload that raises changes nothing: the old system keeps running, and its entities
  keep their ids.
- **reload.AC4**: A headless run does not pick up a changed file by itself.
- **reload.AC5**: Over MCP: `reload` after an edit makes the new behavior visible to `step_frames` and
  `get_component`. A broken edit shows up in `reload` and `list_assets`, and the old behavior goes on.

## Out of scope

- OS file-change notifications (polling is enough for now), reloading assets loaded by other assets
  (dependencies, with glTF), migrating script component data across field changes, and reloading shaders
  (VCH-009).

## Links

- Specs: [asset-system-core](../asset-system-core/spec.md), [scripting](../scripting-luau-api-v0/spec.md),
  [mcp-server](../mcp-server/spec.md)
- Backlog: VCH-004
