# 0052: Procgen runtime: code-first stages and JSON field graphs, CPU-authoritative, GPU visual-only, a Luau procgen sandbox

Date: 2026-11-27

## Status

Accepted.
Its Luau sandbox is amended by [0070](0070-generator-sandbox-rules.md) (`^` rejected, budgets, hook rules).

## Context

Procgen has four workloads: **records** (1 to ~1,000 per body, branchy physics), **coarse maps** (10^5-10^6 samples
once per body, non-local), **fields** (10^6-10^9 samples while streaming, pure per-sample functions) and **scatter**
(10^3-10^6 instances per area). One mechanism fails somewhere: a universal node graph is wrong for records and its
global nodes break tiling (Gaea, World Machine); code-first only means every terrain function exists twice, in C++ and
Slang, and drifts. Anything gameplay uses comes from a deterministic CPU path
([0043](0043-determinism-tiers-and-strict-floating-point.md)); the null RHI must run everything authoritative; Slang
compiles offline ([0009](0009-shader-compilation.md)); Luau today calls libm and has no budgets
([0007](0007-scripting-luau.md)). Research: [05-procgen-graph](../research/05-procgen-graph.md).

## Decision

**One engine area, two mechanisms.** `engine/procgen` (generic, [0042](0042-space-toolbox-layering.md)) owns the stage
runtime, the field-graph IR with its JSON loader and Luau builder, the node registry, the CPU batch interpreter, the GPU
interpreter (a library render features call to add compute passes) and the Luau procgen sandbox. `space/` registers the
toolbox's stages ([0051](0051-staged-generation-pipeline.md)), nodes and kinds.

**Stages (records, coarse maps) are code.** A stage registers id, version, stability class, tier, input record types
(earlier stages only), output record type and a C++ or Luau implementation. Stages are **pure**: no world access, no
command buffer. They run on CPU worker jobs (VCH-015), are cached by (id, stage id, version, input hash, manifest
fingerprint), and fail as `procgen.stage_failed` with nothing partial kept. A mod overrides one stage like a material
overrides one function: `procgen.stage("mymod:climate_locked", { version = 1, extends = "space:body.climate" },
function(ctx, body, default) ... end)`, calling `default` for the public behavior.

**Fields and scatter are data: a JSON field graph.**

- Canonical form is JSON (`"veach_graph": 1`, `id`, `inputs`, `params`, `nodes`, `outputs`); `procgen.graph(...)` in
  Luau builds the same IR. Graphs are assets.
- **Node types are versioned** (`veach:fbm@1`); a changed output is a new version, and old versions stay in the binary
  while any universe uses them ([0055](0055-universe-identity-and-versioning.md)).
- **Node ids are stable strings**; a node's seed is `hash(body_seed, hash_id(node_id))`
  ([0044](0044-counter-based-hash-randomness.md)). Node parameters are reflected structs.
- **Unit classes on every length:** `planet_m` (realized with `s_p` and the relief exponent,
  [0049](0049-scale-model.md)), `m` (never scaled), `system_m` (orbit scale). A bare number is a compile error. Inputs
  `p_planet` (unscaled body-fixed position) and `p_local` (meters on the realized surface). Planet octaves below
  `min_planet_wavelength_m` fade out; octaves below twice the sample spacing are dropped (anti-aliasing and the quality
  knob).
- **Positions arrive split:** integer lattice cell plus float32 offset, computed in double per chunk.
- Coarse maps are inputs through `veach:sample_cubemap@1`, in storage buffers.
- Constant folding, deduplication and one multi-output program per graph (the Factorio model).

**Two evaluators.** The **CPU batch interpreter** (each node processes a SIMD batch) is **canonical**: worker jobs,
headless, `--rhi null`, used for colliders, ground queries and authoritative scatter (anything with collision,
harvested or synced). The **GPU interpreter** (a Slang kernel walking an op list in a storage buffer) is **visual**: far
terrain, grass, debris. Every node type ships a C++ batch kernel and a Slang function, checked by a parity test. GPU
output never feeds gameplay. Mods compose nodes and override stages; per-sample code comes only from C++ modules.

**The Luau procgen sandbox:**

- One sandboxed VM per worker thread; no world access, no command buffers, read-only `require` of the mod's own
  modules, `buffer` for coarse maps.
- Compiled with `disabledBuiltins` listing every libm-backed `math.*`, `math.noise` and `math.random`, closing the
  fastcall bypass; the `math` names are rebound to `veach.dmath`, so `math.sin(x)` is canonical.
- `x ^ y`: `luai_numpow` points at `dmath::pow` engine-wide, or `^` is rejected in generators; VCH-083 decides.
- No `math.random`, `math.noise` or `os.clock`; randomness only from the context's `HashStream`s and `veach.hash`.
- A per-invocation instruction budget through `lua_Callbacks::interrupt` (VCH-034), failing with
  `procgen.budget_exceeded`.
- Interpreted only: no `--!native` until a spike shows Luau codegen bit-identical on arm64 and x64.
- Luau never runs per-sample loops; it declares graphs, rules and content and implements record stages.

**Inspection.** Module-registered MCP tools (VCH-100): `procgen_inspect {id}`, `procgen_eval {graph, node, positions}`
on the CPU, and `procgen_preview {graph, node, region, resolution}`, which renders a map **on the CPU** to a PNG, so
previews work headless and in CI. Editing a graph and calling `reload` invalidates its cache by graph hash.

## Consequences

- One definition of every terrain function; the GPU may differ in ulps only where nobody stands.
- Mods get graphs, stages and kinds with no native code and no way to break determinism.
- Two evaluators per node type to keep in step, paid by one parity test.
- Every node version a universe uses stays runnable: the costliest commitment here.

**Validated by** VCH-082 (CPU interpreter, GPU interpreter and a hand-written kernel agree within 1e-5 on Metal and
WARP; GPU interpreter within 2× of hand-written; CPU ≥ 10 M samples/s/core) and VCH-083 (a climate/biome hook hashes
equal on macOS arm64 and Windows x64; fastcall bypass closed; a runaway hook stops with an error code). **Reopen if**
the GPU interpreter is over 2× hand-written (generate Slang at run time, pulling VCH-009 forward; the IR stays), the
CPU interpreter is well under 10 M samples/s/core (a C++ code generator for engine graphs), or Luau arithmetic isn't
bit-equal across platforms (hooks become non-authoritative).

## Alternatives considered

- **One universal node graph** (Houdini, Unreal PCG): wrong shape for records; global nodes break tiling.
- **Code-first only:** every field written twice, CPU and GPU drift, mods can't add terrain.
- **Script-declared expressions only** (Factorio): no answer for records or coarse maps.
- **Per-sample Luau:** far too slow for chunks, and libm-dependent.
