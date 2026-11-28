# Runtime meshes

**Status:** Implemented
**Area:** render
**Owner:** dev lead

## Summary

Meshes and materials made while the game runs (terrain chunks, generated props, tools) drawn by both renderers like
cooked glTF meshes, without a cook step. A module hands CPU data to the engine's `MeshStore`; the renderer uploads it,
draws every entity that references it, and frees the GPU memory when the mesh is removed.

## Motivation

Terrain is CPU-meshed chunks that are rebuilt on every edit and evicted as the camera moves
([ADR 0053](../adr/0053-terrain-density-field.md)); SP-08, SP-09 and SP-12 (VCH-084, VCH-085, VCH-088) and everything
after them need it on screen. Until now the renderer drew only meshes cooked from glTF files. Backlog: VCH-102.

## Requirements

- A module MUST be able to add, update and remove a mesh (48-byte `MeshVertex`es, indices, one material slot per
  triangle, a material per slot) and a material at run time, from C++, through a service, with no GPU handle.
- Malformed data MUST be rejected with `render.mesh_invalid` and a hint; an id the store does not hold with
  `render.mesh_unknown`. A rejected update MUST keep the previous data.
- The renderer MUST build meshlets with the cooker's function (`build_meshlets`, meshoptimizer), so runtime and
  cooked meshes share one GPU layout; triangles of one slot MUST end up in the same meshlets even when submitted
  interleaved.
- An entity with `RuntimeMeshInstance { mesh }` and a `GlobalTransform` MUST draw in the real-time renderer and in
  photo mode, with the mesh's materials. Photo mode MUST build the BLAS only when it draws the mesh.
- A mesh MUST be uploaded once per change, not per frame; an update or remove MUST retire the old GPU buffers through
  the frame model ([ADR 0030](../adr/0030-frame-and-sync-model.md)), never destroy them under a frame in flight.
- Removing a mesh MUST free all of its GPU resources within a frame, whether or not anything still draws it.
- Ids MUST be deterministic per run and MUST NOT collide with asset ids.
- The store SHOULD be safe to call from any thread.

## API sketch

```cpp
auto& meshes = veach::services(world).get<veach::render::MeshStore>();
const veach::AssetId rock = meshes.add_material({.name = "rock", .base_color = {0.4f, 0.36f, 0.32f, 1.0f},
                                                 .metallic = 0.0f, .roughness = 0.7f});
veach::Result<veach::AssetId> chunk = meshes.add({.vertices = vertices,            // std::vector<veach::MeshVertex>
                                                  .indices = indices,              // 3 per triangle
                                                  .triangle_slots = slots,         // 1 per triangle, or empty
                                                  .materials = {grass, rock}});    // per slot
entity.set(veach::RuntimeMeshInstance{*chunk});
meshes.update(*chunk, std::move(remeshed));  // drawn next frame; the old buffers are retired
meshes.remove(*chunk);                       // GPU memory freed; entities pointing at it draw nothing
// build_runtime_mesh(MeshData) validates and builds meshlets without the store (pure, any thread).
```

Design and its alternatives: [ADR 0072](../adr/0072-runtime-meshes.md).

## Acceptance criteria

- **rtmesh.AC1** — A submitted mesh is validated (empty, partial triangle, index out of range, NaN position, slot
  count mismatch, too many slots are each `render.mesh_invalid`), grouped by slot, bounded, and every triangle is in
  exactly one meshlet within the cooker's limits.
- **rtmesh.AC2** — The store's ids are deterministic and never asset ids; an update keeps the id and never changes a
  snapshot already handed out; unknown ids are `render.mesh_unknown`; ids are not reused; the snapshot pointer is
  unchanged while nothing changes.
- **rtmesh.AC3** — An entity with a `RuntimeMeshInstance` and a transform is extracted as a `MeshDraw` with the
  mesh's materials and world matrix; once the mesh is removed it is not drawn.
- **rtmesh.AC4** — A heightfield patch made at run time and updated on frame 2 (`scripts/runtime_mesh_realtime.luau`,
  `scripts/runtime_mesh.luau`) matches its golden in real time and in photo mode on Metal (`tests/golden/
  runtime_mesh_realtime.png`, `tests/golden/runtime_mesh.png`), and runs clean on the null RHI on every platform.
- **rtmesh.AC5** — On the null RHI, a runtime mesh's six buffers are created on the first frame that draws it, not
  again on quiet frames, replaced once per update (one set alive, with the new size), shared by entities drawing the
  same mesh, and all freed after `remove()`, also on a frame with no camera.

## Out of scope

- Luau bindings for the store: scripts drive C++ modules through components today (the lab's `RuntimeMeshDemo`).
- Sub-range updates and buffer reuse; a persistent snapshot map for thousands of chunks (ADR 0072 says when).
- Per-entity material overrides: materials belong to the mesh.
- DX12 goldens: the path is backend-neutral and the null-RHI test runs on Windows CI; WARP goldens can be added
  with the next Windows-touching change.
- Colliders from runtime meshes (VCH-088), LOD transition masks (VCH-084).

## Open questions

None.

## Links

- ADRs: [0072](../adr/0072-runtime-meshes.md), [0011](../adr/0011-gltf-import-and-cooking.md),
  [0020](../adr/0020-renderer-data-and-extraction.md), [0030](../adr/0030-frame-and-sync-model.md),
  [0040](../adr/0040-interim-realtime-path-visibility-buffer.md), [0041](../adr/0041-photo-mode.md),
  [0053](../adr/0053-terrain-density-field.md)
- Backlog: VCH-102
- Guides: [rendering](../guides/rendering.md)
