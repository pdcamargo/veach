# Golden images

Reference images for `golden: ...` CTest tests (docs/guides/rendering.md). They are stored in Git LFS.

- One shared set for all backends ([ADR 0031](../../docs/adr/0031-image-comparison-and-goldens.md)):
  `<scene>.png`, regenerated on Metal with `cmake --build --preset debug --target update-goldens`.
- Per-backend overrides (`<scene>.<backend>.png`) only with an entry below.

## Per-backend overrides

None.
