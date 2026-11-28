# 0072: Runtime meshes live in a renderer-owned MeshStore of immutable snapshots

Date: 2026-11-27

## Status

Accepted. Implements the runtime-mesh path that [0053](0053-terrain-density-field.md) and
[0060](0060-gpu-driven-visibility-buffer.md) assume (VCH-102, [spec](../specs/runtime-meshes/spec.md)).

## Context

Until now the renderer drew only cooked glTF meshes: `SceneRenderer` looked a `MeshDraw`'s `AssetId` up in the
`AssetServer`, uploaded it once and kept it for the run. Terrain chunks ([0053](0053-terrain-density-field.md)) are
CPU-meshed caches, never saved, rebuilt on every edit and evicted as the camera moves; vegetation and other
generated geometry are the same shape. They need a way into both renderers (real time, [0040](0040-interim-realtime-path-visibility-buffer.md);
photo mode, [0041](0041-photo-mode.md)) without a cook step, with GPU memory freed when a mesh goes away, without
gameplay seeing GPU handles ([0020](0020-renderer-data-and-extraction.md)) and without destroying buffers a frame in
flight still reads ([0030](0030-frame-and-sync-model.md)).

## Decision

- **`render::MeshStore`** (`engine/render/include/veach/render/mesh_store.hpp`), owned by the `Renderer` and provided
  as an engine service. A module calls `add(MeshData)`, `update(id, MeshData)`, `remove(id)`, and the same three for
  `MaterialAsset`s. `MeshData` is the cooked layout: 48-byte `MeshVertex`es, indices, one material slot per
  triangle, and a material id per slot (a store or glTF material). Every method is thread-safe.
- **The store builds, the renderer uploads.** `build_runtime_mesh()` validates (`render.mesh_invalid`), groups
  triangles by slot (a stable counting sort), computes bounds and builds meshlets with `build_meshlets()`, the
  cooker's own function, on the caller's thread and outside the lock. It is pure, so a mesher may call it on a
  worker later. The result is an immutable `RuntimeMesh` behind a `shared_ptr`: an update makes a new one.
- **Ids are `AssetId`s with UUID version 0** (`hi` = "VEACH" + 0, `lo` = a counter), so they never collide with
  asset ids (version 4 or 5), `MeshDraw` and material lists need no new type, and the n-th mesh of a run has the
  same id every run.
- **Extraction copies one pointer.** `MeshStore::snapshot()` returns an immutable `RuntimeAssets` (id to mesh, id to
  material), rebuilt only after a change, so quiet frames cost O(1). The engine's scene extractor stores it in
  `RenderWorld::runtime_assets` and turns each `RuntimeMeshInstance { AssetId mesh }` with a `GlobalTransform` into a
  `MeshDraw` carrying the mesh's materials. The renderer reads nothing else from the store.
- **The GPU cache follows the snapshot.** `SceneRenderer` keys runtime meshes by id, like cooked ones, and keeps the
  `RuntimeMesh` alive while its buffers exist. When the snapshot pointer changes it sweeps: a mesh removed or
  replaced has its buffers (and BLAS) destroyed, which retires them until the frames that read them complete, and
  invalidates the TLAS if it named that BLAS. A changed mesh is uploaded again in full, once, the next frame it is
  drawn; an unchanged mesh is never re-uploaded. BLASes are still built only when photo mode draws the mesh.
- **The store owns lifetime, not the component.** `RuntimeMeshInstance` is a reference: removing it does not free
  the mesh, and an entity pointing at a removed mesh draws nothing. Several entities may draw one mesh.

## Consequences

- Terrain, vegetation and tools put geometry on screen with no renderer changes, through the same meshlet layout the
  GPU-driven path ([0060](0060-gpu-driven-visibility-buffer.md)) will consume.
- An edit re-uploads the whole chunk (on the order of 100 KB for a 32³-cell chunk) and recreates its six buffers.
  That is the right size for 16 m chunks; sub-range updates would need per-slot copies (ADR 0030 rule 3) and wait
  until a measurement asks for them.
- A snapshot rebuild copies the id maps (pointers, not meshes) once per frame that changed something: O(meshes) on
  busy streaming frames. If thousands of resident chunks make that visible in Tracy, switch to a persistent map or
  per-frame change lists.
- Photo mode recreates the TLAS whenever a drawn mesh changes, and restarts accumulation: fine for a reference
  renderer, never used for frame-rate rendering.
- Camera-relative positions ([0045](0045-large-world-positions-and-reference-frames.md)) are unchanged: chunk vertices
  are chunk-relative and the entity's transform places them, so re-basing stays an extraction concern.
- Runtime meshes are not in `list_assets` or saved; they are caches by definition.

## Alternatives considered

- **Runtime entries in the `AssetServer`:** it would mix caches with files that have ids in `.meta`, reload and
  save semantics, and put a renderer concern (meshlets, GPU lifetime) in the asset layer.
- **Mesh data in a component:** large vectors in the ECS, copied by reflection, JSON and MCP, and a change on every
  set; ADR 0020 keeps components small descriptions of intent.
- **GPU handles returned to the module:** breaks ADR 0020 and lets gameplay hold a buffer past its retirement.
- **Rewriting buffers in place on update:** needs a copy per frame in flight to avoid racing the GPU, for a gain
  nobody has measured.
