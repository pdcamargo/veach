# 0084: Amendments from building engine/procgen: per-sample positions, no level-based band limits, two domains

Date: 2026-11-27

## Status

Accepted. Amends [0052](0052-procgen-runtime.md) and the procgen row of [0065](0065-engine-areas-for-the-toolbox.md).
Found while building `engine/procgen` (VCH-112, [spec](../specs/procgen/spec.md), [guide](../guides/procgen.md)).

## Context

`engine/procgen` now holds the stage registry, runtime and cache, the field-graph JSON loader, compiler and canonical
CPU interpreter, a strict-Luau graph builder and the generator sandbox moved out of `engine/script`. Every committed
SP-05, SP-06 and SP-07 hash passes unchanged; the interpreter runs SP-06's terrain at 18 to 19 M samples/s per core
on the M5 Max and 217 M/s on 17 threads with one chunk per job. Building it against
[0074](0074-amendments-from-sp-08.md)'s rule that a sample is a pure function of its position showed two parts of 0052
that contradict that rule.

## Decision

1. **Positions enter as integer LOD0 lattice points**, and each noise octave computes its cell and fraction from the
   point in double, per sample. 0052's per-chunk split into a cell plus float offset would make a shared point depend
   on the chunk asking for it. It costs about 6 % against the float path.
2. **No octave dropping by sample spacing.** 0052's "drop octaves below twice the sample spacing" depends on the
   caller's level. Any band limit is a function of position only; none is implemented today.
3. **Two evaluation domains:** the integer lattice is canonical for terrain and collision; cube-face and
   direction-list evaluation serve coarse maps and previews. The same graph gives slightly different bits in the two
   domains, and nothing canonical mixes them.
4. **Streaming evaluates one chunk per job.** Splitting a 35³ chunk across jobs drops throughput from 12.8 M to 5.3 M
   samples/s per thread.
5. **A stage registry belongs to one universe**: G0 captures the creator settings when it is registered.
6. **procgen depends on `core` only** (Luau and nlohmann privately). It takes `ecs`, `asset` or `script` when a
   feature needs them (graphs as assets, VCH-120). `check_repo.py` fails if procgen or voxel reach `rhi` or `render`.
7. Per-node limits are 16 registers and 24 octaves.

## Consequences

- Seams stay closed by construction: no node can see a level or a chunk.
- The GPU evaluator stays in `tests/sp06` until it moves into `engine/render` as a render feature (0065).
- Follow-ups: VCH-120 (graphs as assets with reload, procgen MCP tools) and VCH-121 (more nodes, a node registry, Luau
  stages, a generator type environment).

## Alternatives considered

- **Per-chunk float offsets (0052 as written):** faster by 6 %, and breaks seams between chunks of different levels.
- **Level-based octave dropping:** cheaper far terrain, the same seam break; far-field savings come from sampling fewer
  points, never from changing what a point evaluates to.
