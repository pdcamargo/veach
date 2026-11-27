# 0021: Git LFS for binaries

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0017).

## Context

Textures, models and golden images are in the tree. A binary committed to plain git history stays in every clone
forever, and migrating history to LFS later is disruptive.

## Decision

- `.gitattributes` tracks with LFS:
  - textures: `*.png *.jpg *.jpeg *.tga *.bmp *.psd *.exr *.ktx *.ktx2 *.dds *.basis`;
  - HDRIs: `*.hdr *.exr`;
  - models: `*.glb *.gltf.bin *.bin *.fbx *.obj *.blend *.usd *.usdz`;
  - audio: `*.wav *.ogg *.mp3 *.flac`;
  - fonts: `*.ttf *.otf`;
  - golden images: `tests/golden/**/*.png` and `*.exr`.
- Text is normalized with `* text=auto eol=lf`, so Windows and macOS checkouts agree. Generated text (`types/`,
  `schemas/`), `.meta` sidecars and `.gltf` JSON stay plain text; only binary buffers go to LFS.
- Contributors run `git lfs install` once. CI checks out with LFS enabled.

## Consequences

- Clones stay small, and history never contains large blobs.
- LFS storage and bandwidth on GitHub need watching as asset volume grows; large local-only assets (Bistro) are
  checked by the pre-push gate, not downloaded by CI ([0034](0034-pre-push-gate-no-gpu-runner.md)).

## Alternatives considered

- **Plain git for binaries:** bloat that can't be undone without rewriting history.
- **An external asset store (S3, Perforce):** a second source of truth; revisit if asset volume outgrows LFS.
- **Adding LFS later:** needs `git lfs migrate` and a history rewrite.
