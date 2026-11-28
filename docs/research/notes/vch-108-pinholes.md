# VCH-108: pinholes and dark thin triangles in the real-time renderer

SP-08 ([findings](../spikes/sp-08-mesher.md)) saw two things along Transvoxel seams in the real-time renderer: sky-colored
pinholes, only when a chunk's transition cells were exported as a second glTF primitive, and short dotted lines of dark
pixels. Both turned out to be the same thing: shading, not coverage. Nothing was missing from the visibility buffer, and
the number of primitives does not matter.

## What the triangles are

Transvoxel leaves a few transition triangles lying in a chunk's face plane, which stands across the terrain. They are
about a pixel wide (one is 1.2 cm), and their vertex normals are the terrain's (up), not the triangle's (sideways). The
camera sees either side of them.

## Root causes

Measured on the SP-08 `look` view by writing debug flags from the shade pass instead of light:

1. **The sun was cut off by the triangle's plane, not by the surface's normal.** The shade pass skipped every light
   behind the triangle's geometric normal (ADR 0040: so a normal map cannot leak light in without shadows). On a
   camera-facing transition triangle that normal points away from the sun while the vertex normals face it, so the
   pixel kept only the sky's ambient light. Under SP-08's constant magenta sky, ambient light on the white second
   primitive is almost exactly the sky's color (111, 14, 111 against 113, 15, 113): those were the "pinholes". With one
   primitive, the same pixels had the level color and showed as dark purple dots, so they were not counted as holes.
   Merging the two primitives into one white primitive gives 36 pinholes on the same view; positions and coverage are
   identical either way.
2. **Seen from behind, the vertex normals were flipped.** `surface_of` (shared with photo mode) turned the interpolated
   normal around with the triangle whenever the back of the triangle was hit. For a triangle standing in a smooth
   surface that turns the terrain's normal into the ground, so the upper dotted line shaded dark. Photo mode showed a
   fainter version of the same line.

Two hypotheses that the measurements ruled out:

- **Meshlet boundaries between material slots, rasterization rules at shared edges.** The visibility pass computes each
  corner from the same vertex values in every meshlet, and the importer copies bit-identical positions per primitive.
  The pinholes were shaded pixels, not cleared ones.
- **Plane barycentrics on thin triangles.** They are ill-conditioned: on the `look` view 525 pixels land more than 1 %
  outside their triangle, 71 more than 10 %, and 3 more than 100 % (the rasterizer's snapped vertices cover pixel centers
  the exact triangle misses). None of them was visibly wrong, since the normals of a smooth surface vary slowly, but they
  extrapolate normals and UVs. They are now clamped to the triangle (no visible change on any golden).

## The fix

In `engine/render/shaders/realtime_shade.slang` and `engine/render/shaders/veach/scene.slang`:

- A **double-sided** material is a thin sheet, and its triangle is the surface: nothing changes (flipped as a whole when
  seen from behind, as glTF says; lights behind the triangle are cut off). Bistro's foliage is double-sided.
- A **single-sided** material bounds a solid, and its vertex normals are the surface: seen from behind, the interpolated
  normal turns only if it faces away from the viewer (both renderers), and the real-time path cuts off lights behind the
  interpolated normal instead of behind the triangle. A normal map still cannot leak light: it perturbs the same
  interpolated normal the test uses.
- The barycentrics of the shaded point are clamped to the triangle.

Side effect, on purpose: on smooth single-sided meshes the light's terminator no longer follows triangle edges, as
photo mode's never did. The real-time goldens `runtime_mesh_realtime` (mean FLIP 0.0036 against the old image),
`flight_helmet_realtime` (0.0017), `bistro_noon` (0.0084, with 549 pixels above 0.5: shutters whose normals face the sun
are now lit) and `bistro_sunset` (0.012) were regenerated. Photo-mode goldens did not move beyond their noise.

## Screenshots (before | after | photo mode)

| Image | What it shows |
|---|---|
| [thin_triangles.png](vch-108/thin_triangles.png) | The new golden's scene (`scripts/thin_triangles_realtime.luau`): 25 cm fins and 1.2 cm slivers standing in a gray ground with its normals and material, sun behind them. Before, they keep only the magenta ambient light, facing the camera or not. After, they vanish, as they must in an unshadowed image. Photo mode shows only their shadows. |
| [sp08_seam_two_primitives.png](vch-108/sp08_seam_two_primitives.png) | SP-08's `look` view with transition cells as a second, white primitive, 3x zoom: the sky-colored dots and the dark line, then nothing. |
| [sp08_seam_one_primitive.png](vch-108/sp08_seam_one_primitive.png) | The same view with one primitive per chunk (before, after): the same two dotted lines in the level color, then nothing. |
| [runtime_mesh_terminators.png](vch-108/runtime_mesh_terminators.png) | The runtime-mesh golden: the old saw-tooth terminators, the new smooth ones, photo mode. |

The golden `golden: thin triangles (realtime)` runs on Metal and DX12 (rt.AC8). Against the old shaders it fails with
mean FLIP 0.111 and 6,636 pixels above 0.5. Its fins and slivers share the ground's material and normals, so a
difference between GPUs in how much of a sliver is covered cannot fail it, only shading can.

## Reproducing

```sh
build/debug/tests/veach-bench-sp08-mesher --only tv --out build/sp08 --look 122.3,-37.1,90.8,128.0,-42.8,90.8
build/debug/apps/lab/veach-lab --project build/sp08/tv --rhi metal --headless --frames 3 --script scripts/look.luau \
    --screenshot look.png
```

Without `--no-highlight` the bench exports transition cells as a second, white primitive; the old pinholes were at
(895..916, 426..433) of the 1280x720 `look` image.

## Left for the mesher

The transition triangles are still there, in the face plane, and a renderer with shadows will draw theirs. SP-08's follow-up stands: a shrink rule at chunk edges that moves them off the face plane, decided
when `engine/voxel` replaces the spike's mesher.
