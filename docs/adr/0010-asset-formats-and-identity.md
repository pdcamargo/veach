# 0010: Asset formats and identity

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0007).

## Context

Agents and humans edit data as text, and every data file must be machine-checkable. References between assets must
survive renames and moves. Loading must be able to become asynchronous without changing call sites. Spec:
[asset-system-core](../specs/asset-system-core/spec.md).

## Decision

**Formats.**

- Scenes and prefabs will be **Flecs Script** (not implemented yet).
- Everything else is **JSON**: asset metadata, import settings, materials, project config. Every JSON format
  corresponds to a reflected C++ type and is read by the reflection-driven reader in `veach/ecs/json.hpp`, with
  did-you-mean errors for unknown fields and enum values.
- The JSON Schema of every format is generated from the same reflection (`veach schemas --out schemas`) and committed
  in `schemas/` (`veach.project.schema.json`, `asset.meta.schema.json`); CTest fails if they are stale. Data files
  may carry a `$schema` key, which the reader ignores.

**Identity.**

- Every source asset has a sidecar `<asset>.meta` (`AssetMeta`: `version`, `id`, `loader`, plus import settings).
- `id` is a UUID v4 (`AssetId`, 128-bit), minted on first load and written next to the file; the `.meta` is committed
  with the asset.
- `veach::Handle<T>` holds only the `AssetId`. Sub-assets inside a source get derived ids
  ([0011](0011-gltf-import-and-cooking.md)).

**API.**

- `AssetServer::load<T>(path)` returns a handle immediately; the same path gives the same handle.
- `get(handle)` returns data or null; `state(id)` and `error(id)` report progress and failures;
  `wait(handle)` returns `Result<const T*>`.
- Loads are synchronous inside `load()` today; the shape is async-ready.

**Loaders** implement `TypedAssetLoader<T>`, claim extensions, receive a `LoadContext` (id, paths, bytes), and must
not touch the ECS world, so they can move to worker threads.

## Consequences

- Moving a file together with its `.meta` keeps every reference valid. Losing a `.meta` mints a new id and breaks
  references; the error hint says so.
- Agents can validate any data file against a schema without running the engine.
- Path-to-id lookup works only for paths loaded in this run; loading by id across a project needs an index of `.meta`
  files.
- No unloading or refcounting yet: assets live until shutdown.

## Alternatives considered

- **TOML / YAML.** Weaker schema tooling (YAML also types ambiguously).
- **Binary metadata.** Not diffable, not editable by agents.
- **Path-based references.** Break on rename.
- **A central asset database file.** A merge-conflict magnet; sidecars keep identity next to the asset.
