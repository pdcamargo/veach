# 0017: Git LFS for binaries

Date: 2026-11-24

## Status

Accepted

## Context

Textures, HDRIs, models and golden images will arrive soon. A binary file committed to plain git history stays in every clone forever. Migrating history to LFS later is disruptive, so the rules must exist before the first binary does.

## Decision

- `.gitattributes` is set up now, before any binary file is committed. Patterns tracked by LFS:
  - Textures: `*.png *.jpg *.jpeg *.tga *.bmp *.psd *.exr *.ktx *.ktx2 *.dds *.basis`.
  - HDRIs: `*.hdr *.exr`.
  - Models: `*.glb *.gltf.bin *.bin *.fbx *.obj *.blend *.usd *.usdz`.
  - Audio: `*.wav *.ogg *.mp3 *.flac`.
  - Fonts: `*.ttf *.otf`.
  - Golden images: everything under `tests/golden/**`.
- Text files are normalized with `* text=auto eol=lf`, so Windows and macOS checkouts agree. Generated text (`types/`, `schemas/`) and `.meta` sidecars are plain text, not LFS.
- Contributors run `git lfs install` once (in `AGENTS.md`). CI checks out with LFS enabled once LFS content exists.
- `.gltf` (JSON) stays plain text. Only its binary buffers go to LFS.

## Consequences

- Clones stay small, and history never contains large blobs.
- LFS needs storage and bandwidth on GitHub. The quota needs watching once asset volume grows.
- A file that matches a pattern but was committed before LFS existed would be a problem. That is why this lands first.

## Alternatives considered

- **Plain git for binaries.** Repository bloat that can't be undone without rewriting history.
- **An external asset store (S3, Perforce).** Overkill now, and adds a second source of truth. Revisit if asset volume outgrows LFS.
- **Adding LFS later.** Requires `git lfs migrate` and a history rewrite. Rejected.
