# 0054: Terrain edits: brush operations on the wire, absolute override samples at rest; water as levels

Date: 2026-11-27

## Status

Accepted.

## Context

Players dig, flatten and build on terrain ([0053](0053-terrain-density-field.md)), and so does simulation (eruptions,
landslides, [0057](0057-simulation-lod.md)). An edit must give the same samples on the server and every client, Mac and
Windows ([0043](0043-determinism-tiers-and-strict-floating-point.md)), travel cheaply
([0056](0056-server-authoritative-multiplayer.md)), survive save, load and generator updates
([0055](0055-universe-identity-and-versioning.md)), and show at every LOD.

The research proposed three storage models: server-ordered op logs with chunk-hash repair (networking), op logs
compacted into relative deltas with a `base_hash` (persistence), absolute override samples per LOD0 chunk (terrain). At
rest, an op log alone breaks the moment the generator or float math changes; relative deltas move every edit with the
ground under it after a generator change; absolute overrides keep edited cells exactly as built. The dev lead's
resolution, conflict 3 in [_decisions](../research/notes/_decisions.md): ops on the wire and in the journal, absolute
overrides at rest. Research: [04-terrain](../research/04-terrain.md) §7.4, [06-networking](../research/06-networking.md)
§2.3, [11-persistence-versioning](../research/11-persistence-versioning.md) §4.4.

## Decision

**The brush operation** is the unit of editing, a versioned record (`kind@version`): `{ kind, shape, center (int64
body-local millimeters per axis), orientation (quantized quaternion), size (mm), strength, falloff, material (u8), tick,
author ("player:<uuid>" or "system:<stage id>"), seq }`. Kinds: `add`, `subtract`, `flatten`, `smooth`, `raise` and
`lower` (along radial up from the toolbox), `paint`. Luau composite tools expand to primitive ops at commit through the
command buffer. Brush size is capped per op (working default 16 m radius, pending the owner's review packet); larger
terraforming is many ops.

**Applying an op (canonical, CPU, integer only):** read the current LOD0 samples (generated and quantized to int16, then
overrides), combine them with the brush in fixed-point integer arithmetic (integer square root for distances), and
write **absolute** override samples with a per-cell mask into every LOD0 chunk the brush touches, apron included.
**Coarse LODs are derived exactly:** coarse samples are a subset of fine ones, so a parent's override is the child
sample at the shared position, propagated up until the edit box is smaller than one cell.

**On the wire and in the journal: ops.** The server assigns a sequence number per body; order is global per body
because flatten and smooth don't commute. A client applies its own op at once and replaces it with the server's result
on acknowledge or reject. Each touched chunk gets a hash of its samples; the server sends hashes for chunks in a
client's interest at a low rate, and on a mismatch or a late join sends the materialized override chunk. Wire
positions are integer cell coordinates; nothing assumes a scale.

**At rest: overrides.** Per LOD0 chunk, a record `{ format, epoch, base_hash, mask, samples (int16 + u8) }`, sparse,
RLE + zstd, in the body's region files (`bodies/<body-id>/terrain/<region>.vreg`, layout owned by
[0055](0055-universe-identity-and-versioning.md)). `base_hash` hashes the chunk's generated, quantized samples at the
body's pinned epoch. The journal holds ops since the last save and is folded into overrides when a save completes. On
load the base is regenerated and checked; a mismatch logs the new error `terrain.base_mismatch` (hint: missing stage
version or a broken determinism contract), and the overrides still apply because they are absolute.

**System edits** (eruptions) are ops authored by `system:<stage id>`, applied by the server, recorded when they happen,
replicated and persisted like a player's.

**Water is levels, not voxels.** Per body an optional sea-level sphere (radius follows `s_p`), and lake levels per basin
from the topography stage's drainage ([0051](0051-staged-generation-pipeline.md)), masked in 2D on the coarse cube map.
Static: dug holes do not flood, caves below sea level stay dry unless inside the ocean mask (working default, pending
the owner's review packet). The generator reads sea level for shore and seabed materials; a render feature draws the
surfaces ([0063](0063-astronomical-rendering.md)). Water-level overrides are a later edit-layer kind.

**Where it lives.** `engine/voxel`: the op record and appliers, override grids, coarse propagation, chunk hashes and the
region-file container as a generic chunked blob store ([0042](0042-space-toolbox-layering.md)). `space/terrain`: radial
up, author ids, sea level and lakes, planetary brush kinds.

## Consequences

- An edit is tens of bytes on the wire and bit-identical on every peer; repair from the server's chunk always works.
- Built things stay where they were built whatever the generator does; with bodies pinned on first edit
  ([0055](0055-universe-identity-and-versioning.md)), a seam at an edit border appears only after an explicit migration.
- Saves hold samples, not history: old op kinds stay runnable only while journals or peers may hold them.
- Water needs no simulation; floods are not a feature.

**Validated by** VCH-088 (brush edits replay to identical chunk hashes on arm64 and x64; a ball falls into the dug hole;
Tracy ms for mesh plus collider per chunk), VCH-096 (two clients dig with the server on macOS and a client in Windows
CI: identical chunk hashes; a corrupted chunk healed by snapshot) and VCH-094 (an anchored body unchanged after a stage
update; save → load → save byte-identical). **Reopen if** VCH-088 shows override records far larger than the op history
in heavy play (keep compacted op logs at rest), or canonical base generation proves unreproducible across platforms
(the server sends edited chunks whole).

## Alternatives considered

- **Op log only at rest:** every load replays history through today's generator.
- **Relative deltas with a base hash:** a generator change moves every edit with the ground under it.
- **Whole chunks on the wire:** 64 KB or more per edit; kept for repair.
- **Water in the density field:** floods need fluid simulation and break the narrow-band encoding.
