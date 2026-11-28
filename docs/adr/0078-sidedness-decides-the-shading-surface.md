# 0078: Sidedness decides the shading surface: vertex normals for solids, the triangle for thin sheets

Date: 2026-11-27

## Status

Accepted. Amends the shade pass of [0040](0040-interim-realtime-path-visibility-buffer.md) ("a light behind the
geometric surface contributes nothing") and the shared surface code photo mode uses. Binds the planned renderer of
[0060](0060-gpu-driven-visibility-buffer.md). Found by VCH-108 ([notes](../research/notes/vch-108-pinholes.md)).

## Context

SP-08's terrain showed dark dotted lines and "pinholes" along LOD seams. The cause was shading, not coverage:
Transvoxel's transition triangles stand upright in a chunk's face plane but carry the terrain's upward vertex normals.
Testing each light against the triangle's own plane cut the sun off from the camera-facing ones, leaving only ambient
light. Seen from behind, the shared surface code also flipped the whole interpolated normal into the ground, which
photo mode showed as a fainter line. Any smooth mesh whose triangles disagree with their vertex normals has the same
problem; on ordinary meshes it showed as terminators that followed triangle edges.

## Decision

1. **A single-sided material bounds a solid, and its interpolated vertex normal is the surface.** Seen from behind,
   the normal is turned only if it faces away from the viewer; lights are cut off behind the interpolated normal,
   not behind the triangle. Normal maps still cannot leak light, since they perturb that same normal.
2. **A double-sided material is a thin sheet, and its triangle is the surface**: flipped as a whole when seen from
   behind (glTF), lights behind the triangle cut off. Foliage and cloth stay as before.
3. **The shaded point's barycentrics are clamped to its triangle**, so thin triangles never extrapolate attributes.
4. Real time and photo mode use the same rule through the shared surface code, and the ADR 0060 renderer inherits it.
   The golden `thin_triangles_realtime` (fins and slivers standing in a ground plane) guards it on Metal and DX12.

## Consequences

- Terrain seams shade like the terrain around them; the dotted lines are gone in both renderers.
- Terminators on smooth meshes follow the normals, as photo mode's always did. Four real-time goldens moved on purpose
  (runtime mesh, Flight Helmet, Bistro noon and sunset; Bistro's shutters that face the sun are now lit).
- A single-sided mesh with badly wrong vertex normals now shows them, rather than having them masked by the triangle.

## Alternatives considered

- **Fix the mesher's transition normals instead:** treats one producer; any mesh with a triangle-normal mismatch
  would still shade wrong.
- **Keep the triangle test and add a bias:** hides thin cases at one scale, and it still darkens terminators.
