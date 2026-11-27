# 0061: Temporal accumulation at native resolution; no upscaling

Date: 2026-11-27

## Status

Accepted.

## Context

The reset ([0001](0001-reset.md)) deleted the temporal upscalers (ours and MetalFX), the temporal history and motion
vectors, because they served the ended ray-tracing plan: they existed to denoise ReSTIR and RT lighting and to render
below output resolution. The real-time renderer is now a PBR rasterizer with no real-time GI or path tracing
([0039](0039-realtime-renderer-is-raster-pbr.md)), built on a visibility buffer that has no MSAA
([0060](0060-gpu-driven-visibility-buffer.md)). Research: [08-renderer](../research/08-renderer.md).

Three things the toolbox needs depend on reuse across frames:

- **Anti-aliasing of foliage and specular highlights.** Without MSAA, subpixel grass and leaves shimmer; FXAA and SMAA
  cannot recover subpixel coverage. No Man's Sky on Mac ships temporal reconstruction for this reason.
- **Ambient occlusion.** GTAO's 0.5 ms at 1080p on a PS4 comes from reusing 6 rotations over frames.
- **Volumetric clouds** (later, out of the first slice). About 20 ms on a PS4 without reprojection and about 2 ms with
  it; every shipped cloud system depends on it.

## Decision

**A native-resolution temporal foundation, and no upscaling.**

- **Native resolution, always.** Every pass that feeds the final image renders at output resolution. No temporal
  upscaler (TAAU), no MetalFX, FSR, DLSS or XeSS, and no ML reconstruction. A spatial render scale (shading below
  output resolution with a non-temporal upsample) is not part of this decision; it is a lever on the M1-class preset
  that waits on the hardware-target packet ([0064](0064-quality-presets.md)).
- **Deterministic jitter.** The projection is offset by a Halton (2, 3) sequence of length 8 indexed by the frame
  counter, never by wall-clock time or a random stream, so record and replay reproduce images on the same machine.
- **Motion vectors** go into the blackboard's `motion` target (`rg16_float` when VCH-072's formats land). Every
  geometry feature that moves writes them; static geometry (terrain) gets camera motion only. The previous frame's
  camera-relative transforms are computed in double at extraction against the current camera
  ([0045](0045-large-world-positions-and-reference-frames.md)), so reprojection stays precise far from any origin.
- **History** buffers are renderer data, outside the ECS ([0020](0020-renderer-data-and-extraction.md)). They are
  invalidated on a camera cut, a teleport or frame switch larger than one frame's motion, a resolution change, and an
  FTL jump; the next frame starts from the current image.
- **TAA** in `post_hdr`, on pre-exposed radiance before display: reprojection by motion vectors, neighborhood clamp in
  YCoCg, luminance-weighted blend.
- **Consumers:** TAA, GTAO's temporal filter ([0062](0062-shadows-ambient-reflections.md)), and later cloud
  reprojection. A new consumer reads history through the same blackboard resources; nothing else keeps its own.

**Why this is compatible with ADR 0039 and the reset.** ADR 0039 rules out real-time GI, real-time path tracing and
any real-time feature that needs ray tracing. This decision adds none of those: it is anti-aliasing and reuse of raster
and compute results, and it adds no light transport. The reset deleted the temporal machinery together with what it
served (denoising RT lighting, rendering below output resolution); what returns here is only the part a rasterizer
needs at native resolution.

**Photo mode stays free of temporal tricks** ([0041](0041-photo-mode.md)): it accumulates path-traced samples while the
camera is still, with no reprojection, no history reuse across camera motion and no denoiser. It never reads the
real-time history buffers.

**Goldens.** A temporal golden renders a fixed number of frames headless with the fixed jitter sequence and compares
the last one by FLIP ([0031](0031-image-comparison-and-goldens.md)). A test with history disabled renders the same
scene, so a regression in reprojection shows as a difference between the two.

## Consequences

- Foliage, specular and AO become affordable at native resolution; clouds become possible later.
- Every geometry feature has to write motion vectors; a feature that forgets ghosts. That is a review rule.
- Ghosting on fast ship flight and thin foliage has to be tuned and judged by eye.
- Cost (estimates): TAA about 0.08 ms on the M5 Max at 1080p, 0.5 ms on an M1-class GPU, 0.35 ms on M3 Pro-class at
  1440p.
- **Validation:** VCH-092 (SP-16): on its vegetation field, side-by-side screenshots with TAA on and off, judged by eye,
  and TAA's ms in Tracy. VCH-090 (SP-14): cockpit to deep space without ghost trails on the cockpit against the sky.
  Cloud reprojection is validated with the cloud feature (a quarter-resolution trace with reprojection at or below a
  fifth of the full cost). VCH-097 (SP-21) measures TAA on real M1-class and M3 Pro-class machines.
- **Reopen if** TAA ghosting on thin foliage or during fast flight is unacceptable by eye after tuning (then MSAA with
  alpha-to-coverage, which reopens [0060](0060-gpu-driven-visibility-buffer.md) in favor of forward), or reprojected
  clouds show artifacts grading cannot hide (2D cloud layers and cheap fog on every preset).

## Alternatives considered

- **No temporal reuse at all.** Foliage shimmers, GTAO costs several times more, and volumetric clouds do not fit any
  budget.
- **FXAA or SMAA only.** Cheap, but they do not recover subpixel foliage.
- **MSAA with alpha-to-coverage.** Needs MSAA in the RHI and per-sample shading at edges in a visibility buffer (Engel:
  shade rose from 3.43 to 15.58 ms with 4x MSAA at 4K); it also does nothing for shader aliasing.
- **Temporal upscaling** (TAAU, MetalFX). The reset removed it on purpose, and native resolution keeps one image path
  for every machine.
