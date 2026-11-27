# 0056: Multiplayer: server-authoritative replication, one world per process with an authority role

Date: 2026-11-27

## Status

Accepted.

## Context

Veach steps at a fixed 1/60 s, reflects every component ([0026](0026-reflection-everywhere.md)) and replays runs with
per-frame state hashes. Multiplayer must decide who is authoritative, what crosses the wire, how edits are ordered, who
receives what across systems, and how two machines prove they run one universe. Only part of the simulation is
bit-identical across platforms ([0043](0043-determinism-tiers-and-strict-floating-point.md)): physics and gameplay are
replay tier. Research: [06-networking](../research/06-networking.md).

Player counts, listen versus dedicated, cross-play and the mod policy are product decisions pending the owner's review
packet. Working defaults from the dev lead: co-op with Mac-Windows cross-play, 4 players on a listen server and 8-16 on
a dedicated one with no cap in any format, dedicated first, mods matching exactly.

## Decision

**Model.** Server-authoritative state replication with client prediction of the local player's movement, the ship it
controls and its own edits. Everything else on a client is interpolated from server state. No lockstep, and no
rollback of the world.

**One world per process with an authority role:** `authority` (single player, listen host, dedicated server) or `proxy`
(client). Single player is the authority path with no connections; a dedicated server is any Veach app run headless on
the null RHI. From the first slice, gameplay systems declare `authority_only` (the default), `predicted` or
`everywhere`. An entity's `authority` (which process simulates it) is separate from its `NetId`, so a process per
system later changes only ownership.

**State tiers.** Every component and generator output is in exactly one:

| Tier | Rule | On the wire |
|---|---|---|
| Regenerated | Function of fingerprint, ids and server tick: orbits, base terrain, flora placement | Nothing |
| Replicated | Server state players share: players, ships, live creatures, items, edit logs, economy | Delta snapshots, events, op logs |
| Cosmetic | Never read by gameplay: particles, far flocks, all GPU output | Nothing |

**Identity.** `NetId` is a server-issued u64. Generated content is addressed by its seed-path id
([0050](0050-universe-addresses-and-seeds.md)). In JSON every 64-bit id is a decimal string
([0046](0046-64-bit-values-across-luau-json-mcp.md)).

**Replication through reflection.** A component replicates when it carries the `Replicated` trait. Members encode by
reflected kind, quantized by the Flecs member range (`.range(min, max)`); orientation as smallest-three (29 bits);
deltas against the last acknowledged baseline. Positions are (frame id, cell key, offset in cell) with cells a fixed
size in meters, so no field width depends on scale; velocities are float32. Script components replicate the same way.
The data plane is binary; the control plane is JSON, and `--net-log` writes decoded packets as JSON lines.

**Edits.** Terrain and system edits travel as the versioned fixed-point brush operations of
[0054](0054-terrain-edits-and-water.md). The server assigns each accepted op a sequence number per body and applies
ops in that order; clients predict their own edits. The server sends chunk hashes for chunks in interest at a low rate;
a mismatch makes it send that chunk's samples. Late joiners get snapshots plus later ops. Simulation edits (eruptions,
[0057](0057-simulation-lod.md)) use the same path.

**Interest.** Frame first: a client receives its own frame chain (system, body, surface or ship interior); other
systems are a roster line. Then fixed-size cells in meters, radii per entity class that never follow scale. Then
priority accumulators under a per-client budget (256 kbit/s default): delayed, never starved.

**Handshake.** The client sends protocol version, engine version, the replicated-schema hash (component and member
names, kinds, ranges) and the universe fingerprint ([0055](0055-universe-identity-and-versioning.md)). The server
answers with the component table and current tick, or refuses with `net.manifest_mismatch` naming the difference and
the fix.

**Transport seam.** A `Transport` interface with reliable and unreliable channels, plus an in-memory transport with
seeded latency, jitter and loss in ticks, so network tests are deterministic. The UDP library (GameNetworkingSockets or
yojimbo) is chosen in VCH-095 and amends [0017](0017-dependency-set.md).

**Clock.** The server owns the universe tick ([0047](0047-universe-clock.md)). Clients run ahead of the server by an
estimate from round-trip time and evaluate orbits locally; orbital elements never cross the wire. No time warp in
multiplayer (working default, pending the owner's packet).

**Debugging.** A server recording captures every client's inputs per tick; a client recording captures received
packets. The state hash doubles as the prediction check. MCP gains `net_status`, `net_entities`, `net_conditions`.

**Placement.** Transport, codec, `NetId`, interest and roles go in `engine/net` when VCH-095 starts; `space/` supplies
the fingerprint's contents. Only the authority declaration is on the first slice's path.

## Consequences

- Cross-play needs bit equality only in the canonical tier; the server corrects everything else.
- Every gameplay system declares its authority from day one: cheap now, a rewrite later.
- **Validation:** VCH-095 (SP-19): headless server and client without visible stutter at 150 ms and 5% loss, a
  manifest mismatch refused with the fix, a client recording that replays. VCH-096 (SP-20): two clients dig, server on
  macOS and one client in Windows CI, identical chunk hashes, a corrupted chunk healed by snapshot.
- **Reopen if** the owner wants hundreds of players in one universe (server meshing becomes founding), listen hosts on
  M1-class Macs cannot hold four occupied places (distributed ownership for co-op), or VCH-096 shows edit application
  differing across platforms.

## Alternatives considered

- **Deterministic lockstep.** Tiny bandwidth, but the whole simulation, physics included, would have to be canonical.
- **Predict and roll back the world.** Out of reach for a streamed planet; co-op needs only local prediction.
- **Distributed ownership** (the nearest player simulates their place). Cheaper servers, but cheating and handoffs.
- **Send voxel data for every edit.** Kilobytes per chunk; kept only as the repair path.
