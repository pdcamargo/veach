# 0028: Metal residency through one residency set; passes name only what the GPU writes

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0044).

## Context

Metal keeps a resource resident for an encoder only when told (`useResource`), and `useResource` also does hazard
tracking for tracked resources, which the render graph relies on ([ADR 0029](0029-render-graph-sync-and-barriers.md)).
Naming every resource a pass declares made a 1,300-mesh scene (Bistro) issue about 45,000 residency requests per
frame: on the M5 Max at 1440p the command buffer took 23.1 ms of GPU time while its passes' timestamps summed to
12.8 ms. Batching the calls into one `useResources:count:` per encoder changed nothing (23.0 ms): the cost is the
GPU making them resident, not the calls. Scenes with few resources showed no gap.

## Decision

On macOS 15 and later the Metal backend keeps **one `MTLResidencySet`** holding every buffer, texture and
acceleration structure it creates, attached to its command queue and committed at submit when it changed.
Resources leave it when destroyed.

`CommandList::use` then skips `useResource` for resources no pass has to order against:

- buffers and textures **no pass has ever written** (mesh data, per-frame uploads, textures once their upload
  completed). A write recorded through `use` marks the resource; from then on every pass names it;
- **BLASes**: shaders reach them only through their TLAS, whose build waits for the BLAS builds, so ordering is
  transitive; a TLAS no longer names its BLASes.

Render targets and anything else the GPU writes keep `useResource` and hazard tracking. On older macOS, or if the
set cannot be created, every declared access is named with `useResource`.

## Consequences

- The scene above dropped from 23.1 to 13.9 ms of GPU time (passes 12.9 ms); small scenes are unchanged.
- A resource's first GPU write must be declared. The render graph always declares writes; a shader writing an
  undeclared resource was a bug before and is a race now.
- Frame-time measurements report the device's frame time (`last_frame_gpu_ms`), not the sum of passes: the gap is
  part of the frame.

## Alternatives considered

- **Batching `useResources` per encoder:** measured, no change.
- **Heaps (`useHeap`):** one call per heap, but every scene resource would come from heaps we size and manage.
- **Untracked resources with explicit fences:** removes tracking cost too, but turns every declared access into a
  backend fence decision. Not needed for this cost.
