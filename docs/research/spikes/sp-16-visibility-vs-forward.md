# SP-16: visibility buffer against forward on a vegetation field (VCH-092)

Validates the core of the planned real-time renderer, [ADR 0060](../../adr/0060-gpu-driven-visibility-buffer.md) (a
GPU-driven visibility buffer for opaque and masked geometry), against its alternative, GPU-driven forward shading with
and without MSAA, on the content 0060 was chosen for: dense alpha-tested foliage. Kill or pivot
([synthesis](../00-synthesis.md)): forward faster, or foliage ghosting unacceptable by eye, means forward with MSAA.
Research: [08-renderer](../08-renderer.md) §2.1, §2.2 and spike S1. Built on indirect draws (VCH-071,
[ADR 0089](../../adr/0089-indirect-work-and-fixed-latency-readback.md)) and the readback queue (VCH-073). Apple M5 Max,
Metal, release build, 2026-11-28.

**Verdict: keep ADR 0060.**

- **The visibility buffer is the fastest path in all 6 view and resolution cases** (16 point lights in the shading):
  forward's best variant without anti-aliasing costs 5 to 36% more, forward with 4x MSAA 22 to 75% more. At 1440p
  the whole frame (culling to display) costs 1.13 to 1.44 ms against 1.46 to 1.55 ms for forward and 1.85 to 1.98 ms
  for forward with MSAA.
- **The margin is the shading cost times the overdraw, so it grows with the shader.** With the cheapest shader we
  could write (sun and ambient only), forward without a prepass is up to 9% faster. With 64 lights the visibility
  buffer is 11 to 36% faster than forward's best variant, and forward with MSAA costs 1.4 to 2.3 times as much. The
  shader 0060 plans (shadow cascades, a light cluster, GTAO, the atmosphere, three textures per material) is heavier
  than the 16-light case of the main table.
- **MSAA is not free on this tile-based GPU even in tile memory:** 4x adds 0.34 to 0.52 ms (27 to 43%) to the forward
  pass at 1440p.
- **Forward kill criterion: not met.** Forward is faster only with a shader far simpler than the renderer's.
- **Raw aliasing, by eye and by FLIP:** both no-AA paths alias the same way (stair-stepped canopy silhouettes,
  crawling grass); 4x MSAA with alpha to coverage smooths silhouettes and the grass field visibly, but not the
  texture aliasing inside the foliage. Against a 16-sample reference it lowers FLIP only from 0.036-0.055 to
  0.030-0.050, and in a slow pan it removes most of the extra frame-to-frame change (shimmer) that no-AA adds.
- **Ghosting criterion: cannot be judged yet.** There is no TAA ([ADR 0061](../../adr/0061-temporal-accumulation-no-upscaling.md)
  has not been built), so there is nothing to ghost. Compared raw, without TAA, both paths alias the foliage the same
  way, and forward with 4x MSAA and alpha to coverage is clearly cleaner at edges (images below). The ghosting question
  moves to the TAA work: if TAA on thin foliage turns out unacceptable, this spike's forward-MSAA path and its numbers
  are the fallback, at 1.2 to 1.75 times the frame cost.

## What was built

| File | What |
|---|---|
| `apps/lab/src/sp16_spike.cpp` | The lab module (`VEACH_SP16=vis\|forward\|prepass`, header comment for every switch): a 512 m field built at start, a render feature with its own culling, both renderers, a 16-sample reference mode, GPU timings and statistics in the log |
| `apps/lab/shaders/sp16/common.slang` | What both renderers share: the records, trilinear sampling, `shade_point` (the engine's glTF BRDF, foliage translucency, N point lights, sky and ground ambient, height fog) |
| `apps/lab/shaders/sp16_cull.slang` | Instance culling (frustum, draw distance, last frame's HiZ) into one compacted list and one `DrawIndexedIndirectArgs` per part; the HiZ pyramid |
| `apps/lab/shaders/sp16_raster.slang` | Every raster pipeline: visibility (opaque, masked), forward (opaque, masked, alpha to coverage), depth prepass, sky, the fragment counter |
| `apps/lab/shaders/sp16_shade.slang` | The visibility buffer's material pass, the display transform (AgX), the 2x2-quad statistics |
| `engine/rhi` (types, Metal, DX12, null, validation) | MSAA, Metal only: `TextureDesc::sample_count` (memoryless attachments), `ColorAttachment::resolve`, `DepthAttachment::resolve`, `GraphicsPipelineDesc::sample_count` and `alpha_to_coverage`. DX12 rejects it with `rhi.not_implemented` |
| `engine/render` (render graph) | `PassBuilder::resolve(multisampled, target)`; the transient pool matches sample counts and mip levels |
| `tests/sp16_msaa_tests.cpp` | `[sp16.msaa]`: validation on every backend; on Metal, a 4x resolve and alpha to coverage |
| `docs/research/spikes/sp-16/scripts/` | `timings.py` (the tables), `stats.py` (culling and fragments), `shots.py` (images, FLIP, shimmer), `lab.py`, `pngio.py` |

### The field

Built procedurally at start, the same for every path: 512 m x 512 m of terrain (131,072 triangles) and **467,767
instances, 8.2 million triangles**:

| Part | Instances | Triangles each | Material |
|---|---|---|---|
| Grass clump: three crossed cards bent outward, 1.3 m x 0.9 m, every 0.7 m | 460,139 | 12 | alpha-tested, double-sided, translucent; drawn to 150 m |
| Tree: trunk | 998 | 120 | opaque bark |
| Tree: canopy of 72 leaf cards | 998 | 144 | alpha-tested leaves |
| Bush: 30 leaf cards | 3,397 | 60 | alpha-tested leaves; drawn to 400 m |
| Rock, two meshes | 1,122 and 2,110 | 1,280 and 320 | opaque |

Textures are 512 x 512 and generated at start (blades, leaf clusters, bark, rock, ground, a normal map for each), with
coverage-preserving mips for the alpha-tested ones, sampled clamped on the cards. When the spike started the Metal
sampler had no mip filter (see Engine findings), so every texture is one texture per mip and both paths filter
trilinearly by hand: two bilinear taps per lookup, the same code in both.

Three views, 60 degree vertical field of view: **ground** (eye 1 m up, inside the grass, looking at a wood), **mid**
(18 m up, over the wood), **far** (90 m up, the whole field to the horizon).

| ground | mid | far |
|---|---|---|
| ![](sp-16/ground_vis.png) | ![](sp-16/mid_vis.png) | ![](sp-16/far_vis.png) |

*The visibility buffer at 1920 x 1080, no anti-aliasing.*

### The two renderers

Both share everything before the visible surface and everything after its radiance:

1. **Cull** (compute, one thread per instance): frustum, draw distance, and an occlusion test of the instance's
   bounding box against **last frame's HiZ** (the farthest depth per texel; reverse-Z), then append to each of the
   model's parts' lists. **Args** writes one `DrawIndexedIndirectArgs` per part (7 parts), instance count from the GPU.
2. **A, visibility buffer** ([0060](../../adr/0060-gpu-driven-visibility-buffer.md)): one render pass, opaque parts then
   masked ones, writing `{instance, part << 20 | SV_PrimitiveID}` into `rg32_uint` with reverse-Z depth; the masked
   variant samples alpha and discards. Then **shade** (compute): per pixel, the triangle is rebuilt from the index
   buffer, barycentrics come from the pixel's ray, UV gradients from the neighbors' rays (as ADR 0040), and
   `shade_point` lights it.
3. **B, forward**, one render pass so depth (and MSAA samples) stay in tile memory:
   - `forward`: opaque parts then masked ones, the full `shade_point` in the fragment shader; a masked fragment runs
     the alpha test first and discards before shading. Opaque fragments get Apple's hidden-surface removal; masked
     ones do not.
   - `prepass`: a depth-only pass first (alpha-tested), then every part again with the full shader and an *equal*
     depth test, no discard: one shade per visible pixel, at the price of a second geometry pass. The two passes use
     different vertex shaders that compute the position with the same code; no pixel was lost (mean FLIP 0.0002
     against `forward` at the mid view).
   - `msaa`: 4x in memoryless attachments, resolved on tile, **alpha to coverage** (alpha sharpened to a pixel wide)
     instead of discard; shading runs once per pixel per triangle.
   - A sky triangle fills what is left (A shades the sky in its compute pass instead).
4. **HiZ** from this frame's depth (11 or 12 small compute passes), then **display** (AgX into `scene_color`).

Shading is the same function in both, so the images match: forward against the visibility buffer, mean FLIP 0.0035
(ground), 0.0055 (mid), 0.0127 (far; the paths pick different triangles for sub-pixel foliage).

## GPU time

Render-graph timestamps (what MCP `gpu_timings` returns), median of 120 frames after 120 frames of warm-up, then the
median of 3 interleaved rounds (`timings.py`). 16 point lights in the shading loop. Total is every pass of the frame
from culling to display.

### Total, ms

| 1920 x 1080 | ground | mid | far |
|---|---|---|---|
| **A: visibility buffer** | **1.10** | **0.98** | **0.90** |
| B: forward | 1.22 | 1.15 | 1.22 |
| B: depth prepass + forward | 1.23 | 1.20 | 1.25 |
| B: forward, 4x MSAA | 1.34 | 1.43 | 1.60 |
| B: depth prepass + forward, 4x MSAA | 1.40 | 1.47 | 1.57 |
| B's best against A, no AA | +11% | +18% | +36% |
| B's best against A, 4x MSAA | +22% | +47% | +75% |

| 2560 x 1440 | ground | mid | far |
|---|---|---|---|
| **A: visibility buffer** | **1.44** | **1.34** | **1.13** |
| B: forward | 1.52 | 1.51 | 1.46 |
| B: depth prepass + forward | 1.55 | 1.55 | 1.46 |
| B: forward, 4x MSAA | 1.85 | 2.01 | 1.97 |
| B: depth prepass + forward, 4x MSAA | 1.96 | 1.98 | 1.87 |
| B's best against A, no AA | +5% | +13% | +29% |
| B's best against A, 4x MSAA | +29% | +48% | +66% |

### Per pass, ms

| 2560 x 1440 | cull + args | visibility | shade | forward | HiZ | display |
|---|---|---|---|---|---|---|
| A, ground | 0.13 | 0.65 | 0.51 | | 0.07 | 0.07 |
| A, mid | 0.13 | 0.55 | 0.52 | | 0.07 | 0.07 |
| A, far | 0.14 | 0.44 | 0.41 | | 0.07 | 0.07 |
| B forward, ground | 0.13 | | | 1.25 | 0.07 | 0.07 |
| B forward, mid | 0.13 | | | 1.25 | 0.07 | 0.07 |
| B forward, far | 0.13 | | | 1.19 | 0.07 | 0.07 |
| B forward + MSAA, ground | 0.12 | | | 1.59 | 0.06 | 0.07 |
| B forward + MSAA, mid | 0.13 | | | 1.75 | 0.06 | 0.07 |
| B forward + MSAA, far | 0.13 | | | 1.71 | 0.06 | 0.07 |

Every pass of every case is in [`sp-16/timings.json`](sp-16/timings.json). The three rounds agree within about 5%;
every comparison is within one sweep. An earlier sweep of the same code, before the clamp fix for the cards' edges
(finding 5), ran 5 to 15% slower in every path (another clock state of the GPU) with the same order and similar
margins.

### How the margin depends on the shader

2560 x 1440, 2 rounds, a separate sweep (so absolute numbers differ slightly from the main table):

| Lights in the loop | View | A | B forward | B prepass | B prepass, 4x MSAA | A against B's best |
|---|---|---|---|---|---|---|
| 0 (sun and ambient only) | ground | 1.22 | 1.12 | 1.32 | 1.64 | +9% (forward wins) |
| 0 | mid | 1.15 | 1.06 | 1.24 | 1.56 | +8% (forward wins) |
| 0 | far | 0.98 | 0.97 | 1.14 | 1.35 | +1% |
| 16 (main table) | ground / mid / far | 1.44 / 1.34 / 1.13 | 1.52 / 1.51 / 1.46 | 1.55 / 1.55 / 1.46 | 1.96 / 1.98 / 1.87 | -5% / -11% / -23% |
| 64 | ground | 2.03 | 2.64 | 2.28 | 2.84 | -11% |
| 64 | mid | 1.96 | 2.89 | 2.53 | 3.46 | -22% |
| 64 | far | 1.66 | 3.15 | 2.59 | 3.81 | -36% |

The visibility buffer pays for its shading once per pixel; forward pays once per masked fragment that survives the
early depth test, or once per pixel plus a second geometry pass with a prepass, and in both cases for whole 2x2
quads. With 64 lights the shade pass costs 0.93 to 1.12 ms; the forward pass 2.0 to 2.3 ms with a prepass and 2.4 to
2.9 ms without. The
renderer 0060 plans shades more than 16 point lights do (four cascades of shadow-map taps, a light-cluster lookup,
GTAO, aerial perspective, three textures per material), so the main table is a conservative estimate of the margin.

### What the views cost in geometry and fragments

`stats.py`, 1920 x 1080 (`VEACH_SP16_STATS=1`):

| View | Visible instances (grass) | Visible triangles | HiZ removed | Masked fragments per pixel | ...that pass the alpha test | 2x2-quad lane use of the visible surface |
|---|---|---|---|---|---|---|
| ground | 36,248 (33,787) | 1.39 M | 8% of triangles | 22.3 | 9.7 | 72% |
| mid | 26,944 (23,334) | 1.74 M | 8% | 11.0 | 4.9 | 58% |
| far | 18,093 (13,420) | 2.16 M | 2% | 2.1 | 0.9 | 43% |

- **Masked fragments per pixel** counts every masked fragment rasterized (a counter in the fragment shader disables
  early depth testing, so this is the depth complexity, not what an early-tested forward pass shades). Forward
  shades somewhere between "pass the alpha test" and all of them, depending on draw order; the visibility buffer
  shades each pixel once.
- **Quad lane use** is the share of the lanes a forward pass spends on covered pixels when it shades only the visible
  surface (after a prepass): at a distance, foliage triangles cover a pixel or two, so most of a 2x2 quad is helper
  lanes. The visibility buffer's compute pass has none.
- **HiZ removes little here:** alpha-tested foliage leaves holes in the depth buffer, so the farthest depth of a HiZ
  texel is almost always the sky or distant ground; it pays on opaque occluders (hills, rocks, buildings). It costs
  0.05 to 0.07 ms. In a sweep without it (1080p) A takes 0.84 to 1.08 ms and B with a prepass 1.21 to 1.27: the
  order does not depend on it.

## Images

`shots.py`: 1920 x 1080, no AA except where stated. The reference is the visibility buffer at 7680 x 4320,
box-filtered to 1920 x 1080 (16 samples per pixel). The crops are 240 x 135 pixels, enlarged 4 times:
**visibility buffer | forward with 4x MSAA and alpha to coverage | 16-sample reference**.

| | FLIP against the reference: visibility buffer | forward | forward, 4x MSAA | frame-to-frame change in a 0.02 degree pan: visibility buffer | 4x MSAA | reference |
|---|---|---|---|---|---|---|
| ground | 0.0360 | 0.0360 | 0.0300 | 0.00967 | 0.00873 | 0.00870 |
| mid | 0.0551 | 0.0552 | 0.0500 | 0.00897 | 0.00775 | 0.00840 |
| far | 0.0467 | 0.0493 | 0.0437 | 0.00755 | 0.00577 | 0.00525 |

The pan's frame-to-frame change is the mean absolute difference of two consecutive frames (red channel, 0 to 1); the
reference's is the real motion, so what a path adds above it is shimmer: without AA +11% (ground), +7% (mid) and
+44% (far); with MSAA +0%, -8% (slightly blurrier than the reference) and +10%.

**Ground: canopy against the sky, and a wood behind bushes and a rock**

![](sp-16/ground_crop0.png)
![](sp-16/ground_crop1.png)

**Mid: the top of a canopy over grass, and canopies on the horizon**

![](sp-16/mid_crop0.png)
![](sp-16/mid_crop1.png)

**Far: trees over the meadow, and the grass's 150 m edge**

![](sp-16/far_crop0.png)
![](sp-16/far_crop1.png)

By eye:

- **Without AA the two paths look the same** (they shade the same function; FLIP 0.004 to 0.013 between them):
  stair-stepped canopy silhouettes against the sky, grass cards that break into single-pixel dots at a distance, and
  in motion they crawl. Shimmer is worst far away, where foliage is thinner than a pixel.
- **4x MSAA with alpha to coverage** smooths every silhouette (canopies against the sky, trunks, rocks) and softens
  the far grass into a texture, close to the reference. It does nothing for the aliasing inside the foliage (leaf
  textures, highlights), which is why FLIP improves by only 10 to 17%: the reference is better there too.
- Neither is acceptable for a shipping look on its own: the visibility buffer needs
  [ADR 0061](../../adr/0061-temporal-accumulation-no-upscaling.md)'s TAA, and forward would need MSAA plus a
  specular-aliasing filter.

The ground view with MSAA, full frame: [ground_msaa.png](sp-16/ground_msaa.png).

## Engine findings

1. **The Metal sampler had no mip filter** (it read mip 0 whatever the gradients, while DX12 filtered trilinearly).
   VCH-098 fixed it on main while this spike ran; the spike keeps its hand-made trilinear filter so the measurements
   stay comparable.
2. **Release builds were broken on main** after VCH-071 (asserts compiled out left parameters unused); also fixed on
   main meanwhile.
3. **Indexed indirect draws with `SV_PrimitiveID` give the visibility buffer its triangle id** on Metal, restarting
   at 0 for every instance (the images would be wrong otherwise). D3D documents the same; unverified here, since
   hosted CI compiles the spike's shaders but runs no SP-16 frame. One draw per part, its instances from a compacted
   list, is enough for foliage; the 0040 path's one draw per instance with a draw instance per meshlet is not needed.
4. **A depth prepass does not pay on Apple's GPU for this content.** Hidden-surface removal already shades opaque
   pixels once, and the prepass doubles the geometry work of 1.4 to 2.2 million visible triangles; it helps only when
   the shader is expensive (64 lights).
5. **Foliage cards need clamped sampling.** With a repeating sampler the bilinear filter blends a card's opaque bottom
   row into its top edge, and alpha to coverage turns that into faint horizontal lines across the image. Material
   samplers for cards clamp.

## What the GPU-driven core needs next (the next task)

In order, for ADR 0060's renderer to replace the interim path:

1. **Culling as the engine's**, not a lab feature: the instance pass and per-part compacted lists measured here
   (0.12 to 0.14 ms for 468,000 instances), with meshlet culling (frustum, normal cone, HiZ) under it for large meshes such
   as terrain chunks and rocks, and **two-phase occlusion** (re-test what last frame's HiZ culled against this frame's)
   so a moving camera never shows a hole for a frame; the spike's static views do not need it.
2. **Opaque first, then masked**, in one render pass (as here); the masked pipeline is a separate draw list.
3. **Material binning:** the spike has one material model and one shade dispatch; the real kernel needs a
   classification pass and an indirect dispatch per bin (0060).
4. **The temporal foundation (0061) is required for foliage**, not optional: without it the visibility buffer
   aliases foliage as visibly as the images show. When it lands, judge ghosting on this field (the spike's views
   and `VEACH_SP16_PAN`) against the MSAA path kept here.
5. **LOD for distant vegetation** (impostors for trees past a few hundred meters, grass density falloff): at the far
   view 43% of a forward pass's lanes would be helpers, and the visibility buffer's raster cost is per triangle.
6. HiZ in one pass (single-pass downsampling) instead of 12 dispatches, when it matters (0.06 to 0.07 ms at 1440p now).

## Decisions to confirm (dev lead)

- **Keep ADR 0060**; no ADR supersedes it. The kill criterion on speed is not met; the one on ghosting is moved to
  the TAA work (0061), where it can be judged.
- **Keep the Metal-only MSAA in the RHI** (small, tested, documented as spike-only) as the fallback should TAA fail on
  foliage, rather than deleting it now. DX12 rejects it; a real implementation there would come with that fallback.
- **The lab module stays** as the vegetation benchmark for the renderer work (it is throwaway code; VCH-097's M3
  Pro run would reuse it).

## Reproduce

From the repository root, release build (`cmake --preset release && cmake --build --preset release`):

```sh
python3 docs/research/spikes/sp-16/scripts/timings.py --lab $PWD/build/release/apps/lab/veach-lab --rounds 3
python3 docs/research/spikes/sp-16/scripts/timings.py --lab $PWD/build/release/apps/lab/veach-lab --rounds 2 \
    --sizes 2560x1440 --lights 64 --paths vis forward prepass prepass_msaa
python3 docs/research/spikes/sp-16/scripts/stats.py --lab $PWD/build/release/apps/lab/veach-lab
python3 docs/research/spikes/sp-16/scripts/shots.py --lab $PWD/build/release/apps/lab/veach-lab \
    --veach $PWD/build/debug/tools/veach-cli/veach --out docs/research/spikes/sp-16 --work /tmp/sp16
```

One frame by hand (from `apps/lab`):

```sh
VEACH_SP16=vis VEACH_SP16_VIEW=ground ../../build/release/apps/lab/veach-lab --rhi metal --headless --frames 4 --screenshot vis.png
VEACH_SP16=prepass VEACH_SP16_MSAA=4 VEACH_SP16_VIEW=ground ../../build/release/apps/lab/veach-lab --rhi metal --headless --frames 4 --screenshot msaa.png
```
