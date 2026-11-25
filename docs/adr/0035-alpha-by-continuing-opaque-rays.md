# 0035: Alpha testing by continuing opaque rays

Date: 2026-11-25

## Status

Accepted. Supersedes the "Alpha" bullet of [ADR 0034](0034-lights-and-reference-path-tracer.md). Visibility rays superseded by [0048](0048-alpha-tested-visibility-rays.md).

## Context

ADR 0034 put alpha testing (`MASK` cutoff, stochastic `BLEND`) in the ray query's candidate loop: non-opaque
geometry reports candidate hits, and the shader commits the covered ones. On Metal (Apple M-series, macOS 26,
Slang 2026.22) this failed twice:

- With the material evaluation inlined in the candidate loop of every `trace()` call site, Metal's shader
  compiler service crashed (`XPC_ERROR_CONNECTION_INTERRUPTED`). A minimal candidate test compiled.
- With any non-opaque instance in the scene, the image changed from run to run, even when every candidate was
  accepted unconditionally. Fully opaque scenes were bit-identical.

The reference path tracer must be deterministic: goldens compare it across runs and backends.

## Decision

**Everything is traced as opaque** (`RAY_FLAG_FORCE_OPAQUE`, opaque BLAS geometry). When the closest hit lands
on a texel of a masked or blended material that does not cover it, the same ray is traced again from the same
origin with `TMin` just past that hit, up to 16 layers. Coverage is evaluated by the generated
`evaluateAlpha(kind, context)`, which calls only `evalBaseColor`. `BLEND` stays stochastic, drawing from the
pixel's RNG.

## Consequences

- Deterministic on Metal and DX12, and one code path for both. The Cornell box golden includes an alpha-masked
  panel, and reruns are bit-identical.
- A ray crossing many transparent layers costs one traversal per layer, which is fine for a reference
  renderer. Beyond 16 layers the ray stops at the 16th surface.
- `TlasInstance::opaque` stays in the RHI for future use (any-hit-style culling), but the path tracer does
  not use it.

## Alternatives considered

- **Keep candidate commits and accept nondeterminism:** breaks golden testing, the reason the reference exists.
- **Ray-tracing pipelines with any-hit shaders:** the natural home for alpha testing, and a second binding
  model (ADR 0034 already rejected it for v0).
