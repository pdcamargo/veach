# Asset system core

**Status:** Implemented
**Area:** asset
**Owner:** dev lead

## Summary

Assets are files under the project directory, referenced through typed handles that hold a stable id. Every
source asset has a `.meta` sidecar storing that id, so renames and moves never break references. Loaders turn
bytes into typed data; the first real loader loads Luau scripts.

## Motivation

Agents move and rename files freely. References that survive this, plus errors that say what to fix, are what
make a text-first project safe to edit. The API has to be shaped for async loading and hot reload from day one,
even though v0 loads synchronously.

## Requirements

- MUST give every loaded source asset a `<file>.meta` JSON sidecar (`version`, `id` as a UUID, `loader`),
  created on first load and never rewritten afterwards.
- MUST reference assets by `Handle<T>`, which holds only the `AssetId`.
- MUST return a handle from `load<T>(path)` immediately, and expose `state(id)`, `get(handle)` (nullptr until
  loaded) and `wait(handle)` (data or the load error).
- MUST resolve paths relative to the project directory and normalize them (`a/../b` is `b`). Loading the same
  path twice returns the same handle.
- MUST fail with stable codes: `asset.not_found` (with a did-you-mean over sibling files), `asset.no_loader`,
  `asset.meta_invalid`, `asset.load_failed`.
- MUST keep loaders away from the ECS world, so they can move to worker threads later.
- MUST validate `.meta` files against `schemas/asset.meta.schema.json` (generated from reflection).

## API sketch

```cpp
auto& assets = veach::services(world).get<veach::AssetServer>();
veach::Handle<veach::ScriptAsset> h = assets.load<veach::ScriptAsset>("scripts/hello.luau");
auto script = assets.wait(h);              // Result<const ScriptAsset*>

class TextLoader final : public veach::TypedAssetLoader<TextAsset> {
    std::string_view name() const override { return "text"; }
    std::span<const std::string_view> extensions() const override;  // {".txt"}
    veach::Result<TextAsset> load(const veach::LoadContext& ctx) const override;
};
assets.add_loader(std::make_unique<TextLoader>());
```

```json
// scripts/hello.luau.meta
{ "id": "1c094eff-ac28-4cce-9e9b-e5e35b22c075", "loader": "luau-script", "version": 1 }
```

## Acceptance criteria

- **asset.AC1** — The first load creates `.meta` with a UUID and the loader name; a later run reads back the same
  id; the same path (normalized) gives the same handle.
- **asset.AC2** — A missing asset fails with `asset.not_found` and a suggestion for a similarly named file.
- **asset.AC3** — Unknown extensions fail with `asset.no_loader`; a malformed `.meta` fails with
  `asset.meta_invalid`.
- **asset.AC4** — The engine's startup script loads through the asset system (covered by script.AC2).

## Out of scope

- Async loading, ref-counting and unloading (VCH-031). Id index and `load_by_id` (VCH-032). Hot reload (VCH-004).
  glTF and textures (done in VCH-051), scenes and prefabs (VCH-027).

## Open questions

None.

## Links

- ADRs: [0010 asset formats and identity](../../adr/0010-asset-formats-and-identity.md)
- Code: `engine/asset/`, `engine/script/src/script_asset.cpp`
