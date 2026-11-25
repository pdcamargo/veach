# 0044: Metal residency through one residency set; passes name only what the GPU writes

Date: 2026-11-25

## Status

Accepted. Supersedes the "Residency" paragraph of [0024](0024-resource-binding-model.md) on Metal.

## Context

ADR 0024 makes every resource a pass declares resident with `useResource` on that pass's encoder, and a TLAS also
makes every BLAS it references resident. A scene pass in Bistro declares about 9,500 resources (6 buffers per mesh,
1,296 BLASes, the textures, and the BLASes again through the TLAS), and five passes read the scene: about 45,000
residency requests per frame. Measured on the M5 Max at 1440p, the command buffer took 23.1 ms of GPU time while
its passes' own timestamps added up to 12.8 ms; batching the requests into one `useResources:count:` per encoder
changed nothing (23.0 ms), so the cost is the GPU making them resident, not the calls. Scenes with few resources
(the Cornell box, FlightHelmet) showed no gap.

`useResource` does two things: residency, and hazard tracking for tracked resources (our render graph relies on
Metal ordering a write in one pass before a read in the next, [0025](0025-sync-and-barrier-model.md)).

## Decision

On macOS 15 and later the Metal backend keeps **one `MTLResidencySet`** holding every buffer, texture and
acceleration structure it creates, attached to its command queue and committed at submit when it changed.
Resources leave it when they are destroyed.

`CommandList::use` then skips `useResource` for resources no pass has to order against:

- buffers and textures **no pass has ever written** (mesh data, per-frame uploads, textures once their upload's
  command buffer completed): a write recorded through `use` marks the resource, and from then on every pass names
  it as before;
- **BLASes**: shaders reach them only through their TLAS, which is rebuilt and named every frame and whose build
  waits for the BLAS builds, so ordering is transitive; a TLAS no longer names its BLASes either.

Everything the GPU writes (targets, history, reservoirs, the TLAS) keeps `useResource` and its hazard tracking. On
older macOS, or if the set cannot be created, the backend behaves as ADR 0024 says.

## Consequences

- Bistro's frame drops from 23.1 to 13.9 ms of GPU time (passes 12.9 ms); scenes with few resources are unchanged.
- A resource's first GPU write must be declared (it always is: the render graph declares writes); a shader that
  writes a resource without declaring it was a bug before and is a race now.
- `frame_times.py` reports the device's frame time (`last_frame_gpu_ms`), not the sum of passes: the gap is part
  of the frame, and vendor work like MetalFX has no per-pass timestamp.

## Alternatives considered

- **Batching `useResources` per encoder:** measured, no change.
- **Heaps (`useHeap`):** one call per heap, but every scene resource would have to be allocated from heaps we size
  and manage; the residency set needs no allocation changes.
- **Untracked resources with explicit fences:** removes hazard tracking cost too, but moves every ordering decision
  into the backend; the render graph's declared accesses would have to become fences. Not needed for this cost.
