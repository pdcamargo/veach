# The GPU-driven core against the interim path (VCH-138)

What the core of [ADR 0093](../../adr/0093-gpu-driven-core.md) ([spec](../../specs/gpu-driven-renderer/spec.md))
costs and saves against the interim path it replaced ([ADR 0040](../../adr/0040-interim-realtime-path-visibility-buffer.md)).
Apple M5 Max, Metal, release builds, 2026-11-28. The interim build is `main` at `64513fd` (the atmosphere of ADR 0092
included) with the same two measurement aids added: the lab's `VEACH_SP16=engine` field and `VEACH_GPU_TIMINGS`.

## How it was measured

`scripts/sweep.py` runs every configuration on both builds in turn, twice, and takes the median of the two rounds'
medians; `scripts/timings.py` is one run, `scripts/tables.py` prints the tables from [`timings.json`](timings.json).

- **GPU ms per pass**: render-graph timestamps, the median over 100 frames (40 for the field, 60 for the slice) after a
  warmup, taken by the renderer itself (`VEACH_GPU_TIMINGS`) while frames run back to back.
- **Why not MCP `gpu_timings` between steps**: an Apple GPU left idle between frames clocks down, and every pass, even
  ones this work did not touch (display, the sky's), then reads two to three times slower. The same happens when the
  CPU is the bottleneck: a frame whose CPU work is long leaves the GPU idle and its passes are timed at lower clocks.
  The CPU time per frame is in the tables for that reason; where the two builds differ a lot in it (the field), the GPU
  numbers mix the work with the clock state.
- **Scenes**: Bistro at noon (`scripts/bistro_noon.luau`'s view); the SP-16 vegetation field through the engine
  (`VEACH_SP16=engine`, views ground, mid and far: 467,767 instances as entities, 8.2 million triangles, the cards in
  FieldGrass and FieldLeaves materials whose coverage comes from UV); the slice landed on the planet at s = 0.01 and
  s = 1 (`VEACH_SLICE_AUTOPILOT=1`, from the frame the autopilot lands, at the slice's own 2560 x 1440).

## Results

GPU ms per frame, render-graph passes grouped (culling: every `cull` pass and its readbacks; HiZ: every
mip; other: the sky's tables, aerial perspective, the backdrop, exposure, display, uploads). Median of two rounds.
CPU ms per frame is the wall time of the frames measured (for the slice, of the whole run).

| Scene | Size | Path | Total | Culling | Visibility | HiZ | Classify | Shade | Other | CPU ms/frame |
|---|---|---|---|---|---|---|---|---|---|---|
| bistro | 1920x1080 | interim | 2.15 | 0.00 | 1.68 | 0.00 | 0.00 | 0.27 | 0.19 | 2.8 |
| bistro | 1920x1080 | gpu_driven | 1.38 | 0.05 | 0.68 | 0.06 | 0.04 | 0.31 | 0.20 | 2.8 |
| bistro | 2560x1440 | interim | 2.48 | 0.00 | 1.78 | 0.00 | 0.00 | 0.47 | 0.23 | 2.8 |
| bistro | 2560x1440 | gpu_driven | 1.70 | 0.05 | 0.78 | 0.08 | 0.07 | 0.49 | 0.22 | 2.8 |
| field_far | 1920x1080 | interim | 33.86 | 0.00 | 33.33 | 0.00 | 0.00 | 0.32 | 0.20 | 102.3 |
| field_far | 1920x1080 | gpu_driven | 7.37 | 0.45 | 5.76 | 0.08 | 0.14 | 0.60 | 0.34 | 31.8 |
| field_far | 2560x1440 | interim | 33.80 | 0.00 | 33.06 | 0.00 | 0.00 | 0.49 | 0.25 | 102.9 |
| field_far | 2560x1440 | gpu_driven | 7.39 | 0.44 | 5.46 | 0.09 | 0.18 | 0.87 | 0.37 | 32.0 |
| field_ground | 1920x1080 | interim | 32.89 | 0.00 | 32.47 | 0.00 | 0.00 | 0.22 | 0.21 | 103.4 |
| field_ground | 1920x1080 | gpu_driven | 7.46 | 0.48 | 5.63 | 0.11 | 0.08 | 0.56 | 0.41 | 31.5 |
| field_ground | 2560x1440 | interim | 33.13 | 0.00 | 32.51 | 0.00 | 0.00 | 0.37 | 0.25 | 102.9 |
| field_ground | 2560x1440 | gpu_driven | 6.97 | 0.45 | 4.93 | 0.14 | 0.12 | 0.90 | 0.48 | 31.9 |
| field_mid | 1920x1080 | interim | 33.16 | 0.00 | 32.65 | 0.00 | 0.00 | 0.25 | 0.21 | 101.9 |
| field_mid | 1920x1080 | gpu_driven | 7.20 | 0.47 | 5.57 | 0.09 | 0.08 | 0.51 | 0.36 | 31.5 |
| field_mid | 2560x1440 | interim | 33.57 | 0.00 | 32.88 | 0.00 | 0.00 | 0.42 | 0.25 | 103.8 |
| field_mid | 2560x1440 | gpu_driven | 7.09 | 0.43 | 5.30 | 0.10 | 0.11 | 0.74 | 0.36 | 31.9 |
| slice_s0.01_landed | 2560x1440 | interim | 3.37 | 0.00 | 1.65 | 0.00 | 0.00 | 0.78 | 0.76 | 11.9 |
| slice_s0.01_landed | 2560x1440 | gpu_driven | 2.89 | 0.08 | 0.56 | 0.16 | 0.14 | 1.04 | 0.94 | 11.9 |
| slice_s1_landed | 2560x1440 | interim | 3.60 | 0.00 | 2.07 | 0.00 | 0.00 | 0.77 | 0.68 | 4.2 |
| slice_s1_landed | 2560x1440 | gpu_driven | 3.63 | 0.10 | 0.78 | 0.19 | 0.16 | 1.29 | 1.05 | 4.2 |

What culling did on the last measured frame (GPU-driven only; meshlets drawn by phase 1 + phase 2):

| Scene | Size | Instances | Visible (phase 1) | Rechecked / disoccluded | Meshlets culled | Meshlets occluded | Meshlets drawn |
|---|---|---|---|---|---|---|---|
| bistro | 1920x1080 | 1296 | 247 | 10 / 0 | 5910 | 1864 | 9379 + 0 |
| bistro | 2560x1440 | 1296 | 245 | 12 / 0 | 5910 | 1927 | 9336 + 0 |
| field_far | 1920x1080 | 467767 | 351871 | 4806 / 0 | 2167 | 3619 | 369972 + 0 |
| field_far | 2560x1440 | 467767 | 352571 | 4106 / 0 | 2172 | 3023 | 371250 + 0 |
| field_ground | 1920x1080 | 467767 | 158549 | 30084 / 0 | 1827 | 10250 | 157409 + 0 |
| field_ground | 2560x1440 | 467767 | 159485 | 29148 / 0 | 1838 | 12163 | 156637 + 0 |
| field_mid | 1920x1080 | 467767 | 242766 | 16527 / 0 | 2145 | 10645 | 247411 + 0 |
| field_mid | 2560x1440 | 467767 | 246254 | 13039 / 0 | 2166 | 8951 | 252610 + 0 |

## Images

GPU-driven, 1920 x 1080: the field's three views. Each is identical to the interim path's image of the
same frame (FLIP 0); culling changes no pixel.

| ground | mid | far |
|---|---|---|
| ![](field_ground.png) | ![](field_mid.png) | ![](field_far.png) |

Every real-time golden (Cornell box, FlightHelmet, runtime mesh, thin triangles, the three frames goldens, blend modes,
space backdrop, Bistro at noon and sunset) renders the same image on both release builds: mean FLIP 0.00000.

## Findings

1. **Bistro's visibility pass is cut by 56 to 60%**: 1.78 to 0.78 ms at 1440p and 1.68 to 0.68 ms at 1080p, the
   images unchanged. ADR 0060's bar was half at 1440p; counting culling and the pyramid it is 1.78 to 0.91 ms (49%).
   Of 1,296 instances 247 pass the frustum, 10 more are found occluded; of their meshlets 5,910 fall to the frustum or
   the normal cone and 1,864 to the pyramid, 9,379 are drawn. The frame goes from 2.15 to 1.38 ms at 1080p and 2.48
   to 1.70 ms at 1440p.
2. **The SP-16 field draws 6x faster on the GPU and 3x faster on the CPU** than through the interim path: 33 ms of
   visibility (467,767 draws) against 5 to 6 ms, and 102 against 32 ms per frame on the CPU. It is still far from the
   spike's own 1.1 ms frame at 1080p: the engine has no draw distance, so the ground view draws 158,000 instances
   (the spike's 150 m grass range drew 36,000) and the far view 352,000; and its frames are CPU-bound at 32 ms, so the
   GPU, busy 7 ms of every 32, runs at lower clocks (its display pass, which this work does not touch, takes 0.055 to
   0.09 ms at 1080p against the interim build's 0.045, whose GPU was busy a third of the time).
   Distance culling and LOD for vegetation, and a static-instance path for the CPU, are what the field needs next.
3. **The slice near the ground**: visibility drops from 1.65 to 0.56 ms (s = 0.01) and 2.07 to 0.78 ms (s = 1) at
   1440p; the frame from 3.37 to 2.89 ms at s = 0.01 and stays at 3.6 ms at s = 1, where shading (1.29 against 0.77 ms)
   and every untouched pass (display 0.22 against 0.15 ms) read slower: with less GPU work in a 4 ms CPU frame, the
   GPU sits idle longer and clocks down. Clock-for-clock (by the display pass) the shading costs about 10% more.
4. **Binning costs about 0.1 ms at 1080p for one material kind** (classify 0.04 ms, shade 0.31 against 0.27 ms in
   Bistro): tiles that show the sky and a surface are dispatched twice, once per bin. It pays when kinds differ in
   cost, which the planned materials will; the per-pixel kind check runs only on mixed tiles.
5. **Two-phase occlusion works and costs little**: phase 2 rechecked 10 Bistro instances and 1,864 meshlets, 4,000 to
   30,000 field instances, and drew what became visible in `[gpudriven.AC4]`'s jump; the pyramid's ten or eleven
   mips cost 0.06 to 0.19 ms (a single-pass build would save most of it).
6. **Apple GPU clocks follow the load**, so GPU timings depend on the CPU's pace: measuring by MCP steps (GPU idle
   between frames) read every pass two to three times slower, as did the GPU-driven build before its CPU cost was
   fixed (9.3 ms per Bistro frame, mostly describing a render graph whose passes each declare thousands of mesh
   buffers). The renderer now times itself while frames run back to back (`VEACH_GPU_TIMINGS`).

## Reproduce

From the repository root, with a release build here and one of the interim path elsewhere:

```sh
python3 docs/research/gpu-driven/scripts/sweep.py --new $PWD --old <interim tree> --rounds 2 \
    --out docs/research/gpu-driven/timings.json
python3 docs/research/gpu-driven/scripts/tables.py docs/research/gpu-driven/timings.json
VEACH_SP16=engine VEACH_SP16_VIEW=far build/release/apps/lab/veach-lab --rhi metal --headless --frames 4 \
    --screenshot far.png   # from apps/lab
```
