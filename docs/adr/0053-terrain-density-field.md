# 0053: Terrain is a signed density field in chunked body-local octrees, meshed on the CPU

Date: 2026-11-27

## Status

Accepted.

## Context

Terrain must cover a 1 km asteroid up to a 12,700 km super-earth, hold planet-scale features that follow `s_p` and
meter detail that never scales ([0049](0049-scale-model.md)), allow caves anywhere, be dug into, stream from orbit to
the ground with no loading screen, and give every peer the same collision surface
([0043](0043-determinism-tiers-and-strict-floating-point.md)). Servers and CI run on the null RHI, so colliders can't
come from the GPU.

One density field meshed volumetrically is what No Man's Sky, Space Engineers and Astroneer ship.
Research: [04-terrain](../research/04-terrain.md).

## Decision

**One signed density field per terrain body**, `d(p) = override(p) if edited else generated(p)`, p a body-local
position in meters in the body-fixed frame ([0045](0045-large-world-positions-and-reference-frames.md)), negative solid.
The generator is a field graph ([0052](0052-procgen-runtime.md)), a pure function of position and band limit, never of
chunk key or LOD: a planet-scale heightfield h(direction) with a cached min/max pyramid, meter detail added to it, and
3D terms (caves, overhangs) only inside a cave band [surface − D_cave, surface + overhang height]; solid below, empty
above the relief maximum. D_cave is in meters, a creator setting (working default 500 m). Chunks the bounds prove empty
or solid are never sampled. Asteroids use a fully 3D generator; gas giants have no `TerrainBody`. Water is not in the
field ([0054](0054-terrain-edits-and-water.md)).

**Chunks.** A body-local Cartesian octree of **32³-cell** chunks. **LOD0 cell = 0.5 m** (chunk edge 16 m), a universe
manifest constant, identical on every hardware tier; level l has cells of 0.5·2^l m. The octree is restricted
(neighbors differ by at most one level) and refined by distance with ratio λ, a quality setting (working values 4 on
M1-class, 6 on M3 Pro-class, [0064](0064-quality-presets.md)). Key: `ChunkKey { BodyAddress body; std::uint8_t level;
std::int32_t x, y, z; }` ([0050](0050-universe-addresses-and-seeds.md)).

**Samples.** int16 signed distance at 1/1024 m, clamped to a ±32 m narrow band, plus a u8 material: 3 bytes per sample,
33³ corner samples per chunk (≈108 KB). Surfaces land to the millimeter.

**Meshing.** Marching cubes with Transvoxel transition faces, behind an `IsoMesher` interface. All six transition faces
are meshed with the chunk and toggled per frame by a 6-bit neighbor mask in the vertex-pulling shader, so a neighbor's
LOD change never forces a remesh. Normals come from the density gradient (one-sample apron). Vertices are
chunk-relative; extraction re-bases chunk origins in double. Meshlets use the [0011](0011-gltf-import-and-cooking.md)
layout through the runtime-mesh path (VCH-102). **Meshes are caches: never saved, never sent.**

**The CPU is authoritative and the only producer in v1**, behind a `ChunkProducer` interface. A GPU producer for levels
≥ 2 comes later as a visual-only accelerator, after VCH-071 and VCH-073. Density for unedited chunks is dropped after
meshing; LOD0 samples are kept around players (125 chunks ≈ 13 MB).

**Streaming.** One priority queue per body ordered by screen-space error, served by a fixed per-frame budget of chunk
jobs (`TerrainStreamingBudget`, per quality tier). **Never a hole:** a parent stays drawn until all its children are
meshed; a backed-up queue lowers the effective λ instead of stalling. Rate contract: new chunks per second
N ≈ 1.5·v near the ground and ≈ 96·v/h above 64 m (λ = 4), so a budget B caps speed at v ≤ B·h/96, the shape of the
travel governor ([0059](0059-gravity-flight-travel.md)), whose constants come from the measured B. Working estimate:
B ≈ 300 chunks/s on a base M1, about 200 m/s near the ground.

**Colliders.** LOD0 CPU meshes become static triangle-mesh colliders per chunk within 2 chunks (32 m) of each dynamic
body's predicted position over the next 0.25 s, built on workers, rebuilt on edit, installed at a deterministic tick
([0058](0058-physics-jolt.md)). The dedicated server meshes LOD0 around every player with no GPU.

**Where it lives.** `engine/voxel` (generic, usable for a flat voxel world, [0042](0042-space-toolbox-layering.md),
[0065](0065-engine-areas-for-the-toolbox.md)): chunk store, octree LOD selection, `IsoMesher` and the meshers,
`ChunkProducer`, edit store, scheduler, `ChunkReady` / `ChunkEvicted` events with a read-only view of samples and mesh,
debug draw. `space/terrain` owns `TerrainBody`, spherical generators and the `s_p`/meters split, radial up, sea level, and
binding a body's frame to a voxel volume.

## Consequences

- Caves, asteroids and planets share one representation; a body 10× larger adds about 3.3 octree levels.
- CPU meshing throughput limits low-altitude speed on M1-class machines: the job system (VCH-015) is on the critical
  path; non-blocking readback (VCH-073) is not.
- LOD0 cell, chunk size, sample encoding and chunk key are in every save and wire message; changing them invalidates
  every edited body. The mesher, λ, the scheduler and GPU acceleration stay cheap to change.

**Validated by** VCH-084 (Transvoxel vs surface nets: no cracks across LOD seams, ms/chunk in Tracy, identical mesh
hashes across runs), VCH-085 (descent at s = 0.01 and 1 and onto a 1 km asteroid: no holes, peak chunks/s within 20 %
across scales and within budget) and VCH-088 (dig and collide). **Reopen if** the CPU can't reach B ≈ 300 chunks/s
scaled to M1 (the GPU far-field producer joins the first slice; VCH-071 and VCH-073 become blockers), far-field
triangle counts break the M1 frame (a CDLOD heightfield for the coarsest levels, same generator), surface nets wins
clearly (switch meshers), or building needs crisp pad edges (dual contouring at LOD0).

## Alternatives considered

- **Heightfield on a sphere** (CDLOD, Outerra): no caves, no sideways digging, no asteroids.
- **Heightfield plus local voxel volumes:** two meshers and a seam between them.
- **Sparse voxel octree, ray-marched:** a second renderer next to the raster one
  ([0039](0039-realtime-renderer-is-raster-pbr.md)), nothing for physics.
- **GPU meshing for colliders:** GPU results on the authoritative path, nothing on a GPU-less server.
