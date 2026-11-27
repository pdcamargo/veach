# 0039: The real-time renderer is a PBR rasterizer: no real-time GI or path tracing

Date: 2026-11-27

## Status

Accepted. A product decision by the owner at the reset ([0001](0001-reset.md)).

## Context

The game Veach serves is a planet-scale space game with a stylized look in the spirit of No Man's Sky: vast terrain,
atmospheres that set the mood, vegetation, oceans, clouds, many objects. Its budget goes to scale and content, and it
has to run on ordinary hardware ([0003](0003-platforms-and-hardware-target.md)). Real-time ray-traced lighting spends
the frame on light transport that a stylized look does not need.

## Decision

- **The real-time renderer is a physically based rasterizer** with a stylized but not cartoony look: the glTF
  metallic-roughness materials of [0035](0035-material-model.md), photometric lights
  ([0036](0036-lights-in-photometric-units.md)), physical exposure and AgX ([0037](0037-exposure-and-agx.md)), plus
  grading and artistic controls on top where the look needs them.
- **Out of scope:** real-time global illumination, real-time path tracing, and any real-time feature that requires
  hardware ray tracing. Indirect light, shadows and reflections use raster and compute techniques.
- **It is built on what exists:** the render graph, extraction, bindless resources, Slang materials and the display
  pass. Planet-scale features (terrain, atmosphere driven by composition, oceans, vegetation, clouds) plug in as
  `RenderFeature`s; how is open question 7 of the research phase, as is forward+ versus visibility buffer.
- **Reference images still come from photo mode** ([0041](0041-photo-mode.md)), which shares the materials, so a
  material renders the same surface in both.

## Consequences

- The renderer's quality is judged by eye and by goldens, not against a path-traced ground truth: a stylized raster
  image is not meant to converge to photo mode's.
- The interim real-time path ([0040](0040-interim-realtime-path-visibility-buffer.md)) is deliberately basic until the
  research phase designs the real one.
- Reintroducing ray-traced lighting in real time later would need a new product decision.

## Alternatives considered

- **Keep the raytracing-first renderer.** Ended by the owner: it served a different product.
- **A hybrid (raster with optional RT shadows or reflections).** Two lighting paths to keep consistent and tune;
  possible later as an optional enhancement, but not part of the baseline.
- **A non-physical, purely artistic shading model.** Loses consistency with photo mode and the photometric units
  every light and sky already uses; stylization goes on top of PBR instead.
