# 0100: Sun shadow culling: scene residency in one graph group, casters culled by this frame's receivers and by the light's occlusion, far cascades kept while the scene is still

Date: 2026-11-28

## Status

Accepted. Amends [0095](0095-sun-shadows.md) (decisions 2 and 3, and its cost) and the render graph of
[0029](0029-render-graph-sync-and-barriers.md) (a read-only group). Found in VCH-143 ([spec](../specs/sun-shadows/spec.md),
[findings](../research/sun-shadows/README.md#vch-143-culling-and-caching)).

## Context

Sun shadows (0095) cost far more than ADR 0062's 0.4 ms estimate: on the M5 Max, Bistro +1.0 ms at noon and
+2.5 to 2.8 ms at sunset, the SP-16 field +1.7 to 2.2 ms, and 1.7 to 2 ms more CPU per Bistro frame. The GPU cost is
geometry: drawing a cascade costs the same at 1024² and 4096² (measured), and a 3° sun sweeps every caster of the town
into every cascade (25,000 to 48,000 meshlets each, against the camera's 9,400). The CPU cost was the render graph:
every pass that read the scene declared each of its ~3,000 mesh buffers and textures, and compile, describe and
execute walked them pass by pass. The design target is the M3 Pro at 1440p60 (0066), roughly a third of an M5 Max.

## Decision

1. **Scene residency is declared once per frame.** `RenderGraph::import_group(members, name)` brings many resources no
   pass writes in as one read-only resource: a pass reads it as one access, the graph orders and barriers nothing for
   it, and reading it in another state or writing it is `render.graph_invalid`. `rhi::CommandList::use_group(id,
   members, state)` names the members once per pass; Metal resolves which of them need naming (with the residency set,
   usually none) once per command list, keyed by the group's process-wide id. The scene renderer imports every mesh
   buffer as one group and every texture as another.
2. **Casters are culled by this frame's receivers.** After the camera's visibility pass, each 16x16 tile of its depth
   bounds its receivers by the piece of the view frustum between its pixel centers' rays and its depth range; on every
   cascade that tile looks up (by view depth, blend bands included), the box of those eight corners, widened by the
   lookups' reach (the widest PCF disk and blocker search, bilinear taps, the normal offset: 10 texels), marks a grid
   with a texel per 16x16 map texels with the receivers' smallest depth from the light (minus the normal offset). A
   tile whose box is wider than 4 grid texels (a depth edge) marks pixel by pixel. The grid is a min pyramid
   (`shaders/shadow_pyramids.slang`). A caster whose nearest point to the light is farther than every receiver in
   its footprint, or whose footprint has none, is not drawn (`cull_receivers`, in `gpu_cull.slang`). Exact for the
   frame: nothing the shading reads is dropped. The cascades' culling now waits for the camera's depth instead of
   running beside the camera's culling.
3. **Casters are culled by occlusion from the light, in two phases** as the camera's (0093): phase 1 tests last
   frame's occluder pyramid of the cascade through last frame's cascade matrix moved into this frame's render space,
   draws what passes and rechecks the rest; the occluder pyramid is rebuilt from phase 1's map (the smallest depth per
   16x16 texels, only where receivers are: elsewhere it holds FLT_MAX, since nothing there is read); phase 2 draws
   what it shows. A cascade's depth now reaches the shadow distance toward the light (`fit_cascade`'s
   `toward_light`), not twice its radius, so distant casters keep their depth order instead of all flattening onto the
   near plane, where none could hide another; depth32 float keeps a 1 km range under 0.1 mm.
4. **The shading skips the filter where a lookup is wholly in shadow**: if the occluder pyramid shows every texel the
   blocker search and the widest disk can read nearer the light than the receiver, `cascade_shadow` returns 0, exactly
   what its 32 taps would have found (`ShadowFrame::occluders`).
5. **The far cascades are kept while nothing changes.** The last two cascades of three or four have spheres 1.125 times
   their slice's, always. When the scene is still since the frame before (`GpuScene::still`: no record changed, came or
   went in the same render frame; no material changed), the render frame and the lights too, such a cascade is drawn
   whole (without the receiver test, its receiver planes widened to its sphere) and then not drawn again while the
   slice's own sphere and the lookups' reach stay inside it. Otherwise it is culled and drawn like the others. A scene
   where something always moves redraws them every frame: splitting static from dynamic casters needs instance slots
   that survive entities coming and going, which the GPU scene does not have (VCH-139's static-instance path).
6. **Opaque casters fetch only their position**, and every caster only its own corner's vertex index
   (`shadow_vs_opaque`, `meshlet_corner`).
7. `VEACH_SHADOW_NO_RECEIVERS=1`, `VEACH_SHADOW_NO_OCCLUSION=1` and `VEACH_SHADOW_NO_CACHE=1` turn each part off, to
   compare.

## Consequences

- On the M5 Max, against the build before (both with ambient occlusion and TAA), what shadows add to the GPU frame:
  Bistro at sunset +0.43 ms at 1080p and +0.55 at 1440p (from +2.4 and +2.5), at noon +0.54 and +0.63 (from +1.0);
  the cascades draw 300 and 5,600 meshlets at sunset instead of 26,000 to 48,000. What is left is serial after the
  camera's visibility pass: the receivers pass (0.08 to 0.14 ms), the culling chain, and the two near cascades. Three
  cascades instead of four add +0.42 ms at sunset and +0.49 at noon at 1440p. The images are the build before's: FLIP
  0.00000 to 0.00031 on Bistro still, walking, flying and with the sun moving, frame by frame; 0.0007 on the SP-16
  field, from the far cascades' 12.5% wider texels. No golden changed.
- The CPU no longer pays per scene resource per pass: in a Bistro frame the graph's compile and describe fell from
  about 60% of the render thread to 1%, and an unpaced Bistro frame from 4.8 ms to 1.25 (0.87 without shadow maps).
- The SP-16 field's far cascades (106,000 and 117,000 meshlets of grass with the receiver test alone) cost nothing while
  the field is still; phase 2's instance recheck (random record reads) then dominates its shadow culling.
- Memory: two pyramids per cascade (21,845 uints each at 2048²: 0.7 MB for eight cascades), a recheck list per
  cascade (4 bytes an instance, 8 a meshlet) and twice the draw-list space.
- The M3 Pro (an 18-core GPU, roughly a third of the M5 Max's 40) would pay roughly 1.3 to 1.9 ms for Bistro's
  shadows at 1440p, scaling these numbers: about 1.3 to 1.5 ms with three cascades of 2048², the `high` preset of
  0064, above 0062's 1.1 ms estimate. The receivers pass and the near cascades are what remains to cut there.
  Unmeasured until VCH-097.
- Revisit if: the receivers pass shows in a profile (it costs about as much as the near cascades at noon), a scene
  always moves (static/dynamic split), a later feature needs whole maps (volumetric shadows read them where no pixel
  is: they would turn the receiver test off for their cascades), or the M3 Pro misses its budget with three cascades.

## Alternatives considered

- **Occlusion from the light alone, keeping the cascades beside the camera's culling:** measured 0.2 to 0.35 ms slower
  than with the receiver test, in Bistro at noon and at sunset.
- **Receivers from last frame's depth** (keeps the overlap): wrong the frame the camera turns; a dropped caster is a
  hole in a shadow.
- **One phase of light occlusion against last frame's map:** a caster revealed this frame would be missing for a frame.
- **Updating far cascades every N frames whatever moves:** a moving object's shadow lags N frames; a clipmap that
  scrolls the kept map as the camera moves: more machinery than a 12.5% wider sphere for the same frames saved.
- **A pixel per thread marking the grid with atomics, or 8x8 tiles:** 0.2 to 0.34 ms; the 16x16 tile bound is 0.14.
- **8x8 texels per grid texel:** the same meshlets drawn (within 2%) for twice the pyramid work.
- **depth16 maps or smaller maps:** the cost is vertices, not texels (1024² and 4096² cost the same).
- **Metal heaps or a residency set per frame for the scene:** the residency set already covers it; the cost was the
  graph's work per access, which a group removes on every backend.
