# 0007: Asset formats and identity

Date: 2026-11-24

## Status

Accepted

## Context

Agents and humans edit data as text, and every data file must be machine-checkable. References between assets must survive renames and moves. Loading must be able to become asynchronous (and later hot-reloadable) without changing call sites. See [specs/asset-system-core](../specs/asset-system-core/spec.md).

## Decision

**Formats.**

- Scenes and prefabs are **Flecs Script**. This is not implemented yet (backlog).
- Everything else is **JSON**: asset metadata, import settings, materials and project config.
- Every JSON format corresponds to a reflected C++ type. It is read with the reflection-driven reader in `veach/ecs/json.hpp`, which gives did-you-mean errors for unknown fields and enum values.
- The JSON Schema of every format is generated from the same reflection: `veach schemas --out schemas`. Generated schemas are committed in `schemas/` (`veach.project.schema.json`, `asset.meta.schema.json`), and CTest fails if they are stale. Data files may carry a `$schema` key, which the reader ignores.

**Identity.**

- Every source asset gets a sidecar `<asset>.meta` (`AssetMeta`: `version`, `id`, `loader`).
- `id` is a UUID v4 (`AssetId`, 128-bit), minted the first time the asset is loaded and written next to the file. The `.meta` is committed with the asset.
- `veach::Handle<T>` holds only the `AssetId`.

**API.**

- `AssetServer::load<T>(path)` returns a handle immediately; the same path gives the same handle.
- `get(handle)` returns data or null, and `state(id)` / `error(id)` report progress and failures.
- `wait(handle)` returns `Result<const T*>`.
- v0 loads synchronously inside `load()`. The shape is async-ready.

**Loaders.**

- Loaders implement `TypedAssetLoader<T>`, claim extensions, and receive a `LoadContext` (id, paths, bytes).
- They must not touch the ECS world, so they can move to worker threads.
- The one real loader in v0 is `luau-script` (`ScriptLoader`, `.luau`), which compiles at load time.

## Consequences

- Moving a file together with its `.meta` keeps every reference valid. Losing a `.meta` mints a new id and breaks references, and the error hint says so.
- Agents can validate any data file against a schema without running the engine.
- Path-to-id lookup works only for paths loaded in this run. Loading by id across the project needs an index of `.meta` files (backlog).
- No unloading or refcounting yet: assets live until shutdown.

## Alternatives considered

- **TOML / YAML.** Weaker schema tooling (YAML also has ambiguous typing). JSON Schema is the most widely supported validator format.
- **Binary metadata.** Not diffable, not editable by agents.
- **Path-based references.** Break on rename. Rejected.
- **A central asset database file.** A merge-conflict magnet. Sidecars keep identity next to the asset.
