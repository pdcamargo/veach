# 06: The networking model

Research for question 6 of the research phase ([00-scope.md](00-scope.md)). Raw notes and every source:
[notes/06-networking-notes.md](notes/06-networking-notes.md). Persistence and generator versioning are Q11, numeric
determinism across platforms is Q12; this document covers only what networking needs from them.

**Short answer.** A server owns the truth and sends clients what changed near them; clients predict only their own
character, ship and edits. The universe itself (systems, orbits, base terrain, flora, species) never crosses the
wire: both sides regenerate it from the seed, the manifest and the server's clock. Player edits cross the wire as a
server-ordered log of small operations, checked by hash and repaired by snapshot. A dedicated server is a headless
Veach app. No lockstep, no peer-to-peer.

## 1. The problem in Veach's terms

Veach simulates at a fixed 60 Hz with a `uint64` step counter (`Time::fixed_step`), seeded `veach::Random`, no wall
clock ([ADR 0018](../adr/0018-engine-lifecycle-and-phases.md)). Every component is reflected and one walker
serializes them ([ADR 0026](../adr/0026-reflection-everywhere.md)). Scripts and MCP write through transactional
command buffers ([ADR 0007](../adr/0007-scripting-luau.md)). Record/replay already makes a run a function of
seed + script + inputs, and checks a per-frame state hash ([record-replay](../specs/record-replay/spec.md)). The
networking model has to decide:

1. **Authority.** Who decides what happened: a server, a host player, every peer in lockstep?
2. **What crosses the wire.** Inputs, state, edits, or nothing (regenerated)?
3. **Edits as deltas.** How terrain digs, building and harvesting are ordered, merged and late-joined.
4. **Interest.** Four players on four planets in three systems: who receives what?
5. **Fit.** How it reuses the fixed step, reflection, command buffers, record/replay, MCP and headless runs.
6. **Wire format.** Versioned, checked against the universe manifest on connect, and independent of the scale s.

The (a) items 00-scope assigns to Q6: the universe manifest handshake, the server-owned clock, interest management
across frames, ids as strings on JSON wires. Related (a) items it shares: moving local frames (walking in a flying
ship, with Q1/Q9), FTL between systems (Q10/Q3), authority for economy and factions (Q7), time warp policy (Q2).
Anti-cheat is (c): server authority covers the basics.

What today's engine is missing for networking (read from the tree):

- **Identity.** `CommandBuffer` commands and Flecs entities use world-local `ecs::entity_t`; component ids are local
  too. Nothing can go on the wire as is.
- **Binary serialization.** The walker writes JSON. There is no compact encoding and no quantization.
- **Change detection outside command buffers.** C++ systems write components directly; only scripts and MCP go
  through command buffers.
- **Containers in reflection.** Arrays, vectors and maps are `unsupported` (ADR 0026), and inventories, block lists
  and edit logs need them.
- **A server-side terrain path without a GPU.** A headless server runs on the null RHI, so anything the server must
  know (collision, resource placement) has to come from CPU code.

## 2. Options with precedents

### 2.1 Synchronization model

| Option | What it is | Precedents |
|---|---|---|
| **A. Deterministic lockstep** | Every peer simulates the whole world; only inputs are sent; peers wait for everyone's input each tick | Factorio: "each of the networked peers is running the simulation of everything" and "the game speed is limited by the slowest player" ([FFF #76](https://factorio.com/blog/post/fff-76)); [Fiedler, Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/) |
| **B. Deterministic predict/rollback** | Lockstep inputs, but clients run ahead on predicted inputs and re-simulate the world when real inputs arrive | Photon Quantum, Q48.16 fixed point everywhere ([docs](https://doc.photonengine.com/quantum)); GGPO-style fighting games |
| **C. Server-authoritative state replication + local prediction** | The server simulates; clients get prioritized, delta-compressed state of what is near them; the local player's movement is predicted and reconciled; others are interpolated | Overwatch (predicts movement, abilities, weapons; rolls back and replays only movement; [summary](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback), secondary); Space Engineers hierarchical prediction ([GDC 2023](https://gdcvault.com/play/1028839/Online-Game-Technology-Summit-Predicted)); Unreal replication and Iris ([docs](https://dev.epicgames.com/documentation/unreal-engine/introduction-to-iris-in-unreal-engine)); [Gambetta](https://gabrielgambetta.com/client-side-prediction-server-reconciliation.html); [Fiedler, state sync](https://gafferongames.com/post/state_synchronization/) |
| **D. Distributed ownership** | Each object is simulated by one peer (often the nearest player) and relayed | Valheim: the first player in a zone simulates it until they leave ([Edgegap](https://edgegap.com/blog/valheim-multiplayer-game-backend-deep-dive), community analysis); Elite Dangerous P2P instances with server matchmaking (forums, unverified) |

**A and B are out.** Lockstep has two preconditions and Veach fails both:

- *Everyone simulates everything.* With players in different systems, every client would have to generate terrain,
  colliders and creatures for every occupied planet. Interest management is impossible by construction.
- *Bit-exact determinism of the whole simulation* across macOS arm64 and Windows x64, including user Luau and the
  physics engine (Q9). Fiedler calls cross-platform float determinism "almost impossible to guarantee"; Factorio,
  which controls its whole stack, still fights mod desyncs ("mods, if written incorrectly can cause desyncs very
  easily", [wiki](https://wiki.factorio.com/Desynchronization)). Veach's product is *other people's* Luau mods.

B adds whole-world re-simulation on every late input, which a streamed planet with physics cannot afford.

**D is a later optimization, not a base.** It offloads a weak host, but a laggy or cheating owner breaks the zone for
everyone, and authority moves every time players walk around. Veach should keep an `authority` field per entity so
that D (or server meshing) can be added, and start with the server owning everything.

**C is the recommendation.** It needs determinism only where we choose: the local player's predicted movement (same
code, same platform as the server most of the time; corrected when not), and edit application (made deterministic
by construction, section 2.3). Everything else is "the server said so".

### 2.2 Topology

| Option | Notes |
|---|---|
| **Dedicated server** | A headless Veach process (`--headless --rhi null`) plus a net module. Veach is headless-first, so this is the cheapest topology to build and the only one CI can test end to end. Space Engineers ships both dedicated and in-game servers ([VRAGE](https://keenswh.com/vrage)) |
| **Listen server** | One player's game is also the server. Same authority code, plus a local player. Good for co-op among friends; limited by the host's CPU and uplink (section 6) |
| **Integrated server for single player** | Single player is a listen server with no clients. Minecraft moved to this in 1.3 so single player and multiplayer stopped drifting apart, at a resource cost ([wiki](https://minecraft.wiki/w/Java_Edition_1.3.1/Development_versions)) |
| **Peer-to-peer mesh / host migration** | Elite-style instances (unverified detail). Hard NAT problems, no ground truth to roll back to, host migration needs every peer to hold server state |
| **Server meshing** | Star Citizen: per-entity authority on many server nodes, a replication layer that runs "very little code", a graph database of entity state ([CIG Q&A](https://api.star-citizen.wiki/comm-links/18397)). Built for an MMO, with a dedicated backend tier; not our product now |

**Recommendation: one process, one world, a role.** Every Veach process has one world (AGENTS.md forbids new worlds
in user code) and an authority role: `authority` (single player, listen host, dedicated server) or `proxy`
(client). Single player is the authority role with no connections, so there is one simulation code path, as in
Minecraft, but without a second world or a second process. A listen host is the authority role with a local player
and open connections; a dedicated server is the authority role, headless, with no local player. No P2P, no host
migration.

### 2.3 Edits as deltas

Precedents: NMS stores terrain edits as a capped buffer per save; edits from other players' bases flood it and
"terrain regrows" past the limit (community reports; Synthesis notes that base edits "are now protected from
regeneration", [nomanssky.com](https://www.nomanssky.com/synthesis-update)). Minecraft sends whole chunk sections
(4096 paletted block states each) and then block changes ([wiki](https://minecraft.wiki/w/Java_Edition_protocol/Chunk_format)).
Star Citizen persists every networked entity in a graph database ([CIG](https://api.star-citizen.wiki/comm-links/18397)).

| Option | Bytes per edit | Late join | Determinism needed | Verdict |
|---|---|---|---|---|
| Send voxel data of changed chunks | KB per chunk (a 32³ chunk with 2 bytes per voxel is 64 KB raw) | Send chunks | None | Simple, heavy; right as a repair path |
| **Send operations** (brush kind, frame, center, radius, strength, material, tick, author) | ~40-60 bytes (estimate) | Send the op log, or a compacted snapshot plus later ops | Brush application must be bit-identical | **Recommended**, with hash checks and snapshot repair |
| Send final surface heights (2.5D) | Small | Small | None | Can't express caves; out |

**Ordering and conflicts.** The server assigns each accepted op a sequence number per body (a log per body, or per
edit region later). Order is the server's arrival order; two players digging the same spot simply apply in that
order. Additive brushes commute; flatten and smooth don't, which is why order must be global per body. The client
shows its own edit immediately (predicted), and replaces it with the server's version when the op is acknowledged
or rejected (permissions, NMS-style protected bases).

**Determinism of applying ops.** Density and material are stored as integers or fixed point, and brushes are applied
on the CPU with integer arithmetic, so the result is the same on arm64 and x64 without trusting libm (Q12). The base
density underneath can differ slightly between platforms (float noise); that only matters for predicted collision,
which the server corrects. Every chunk with edits carries a hash; the server sends chunk hashes for the chunks a
client has in interest at a low rate, and a mismatch makes the server send that chunk's voxel snapshot. *Regenerate,
verify, repair.*

**The same path serves system edits.** Eruptions, erosion events and NPC construction (Q7) are ops authored by the
server, so they replicate and persist exactly like player edits.

### 2.4 What is regenerated, what is replicated, what is cosmetic

Three tiers, and every component or generator output belongs to exactly one:

| Tier | Rule | Examples | On the wire |
|---|---|---|---|
| **Regenerated** | A pure function of (manifest, ids, server tick) | Galaxy, systems, orbits, rotation, time of day, seasons (Q2); base terrain density (Q3/Q4); flora placement; species definitions; names | Nothing, after the manifest and the clock are synced |
| **Replicated** | Server state that players can affect or observe in common | Players, ships, creatures near players (position, AI state, health), dropped items, bases and ship blueprints, harvested-flora sets, the edit logs, weather events, economy | Delta-compressed state, reliable event streams, op logs |
| **Cosmetic** | Client-local, may differ between clients, never read by gameplay | Distant bird flocks and fish schools, particles, cloud detail, foliage sway, GPU-only output | Nothing |

- **Creatures:** spawn tables and species look are regenerated (send a species id and a seed, never a mesh);
  individuals within interest of any player are spawned and simulated by the server and replicated. Ambient life
  beyond interaction range is cosmetic.
- **Flora:** placement is regenerated; harvested or destroyed instances are an edit (a set of removed instance ids
  per chunk). That needs instance ids stable across platforms and generator versions: (chunk key, generator
  version, index) from Q3/Q11.
- **Orbits and time:** the server owns the tick. Clients estimate the server tick from round-trip time (Overwatch
  runs the client ahead by RTT/2 plus a buffered frame) and evaluate orbits locally. Orbital elements never cross
  the wire. No time warp in multiplayer (00-scope recommendation, confirmed here: one shared clock).
- **Ships and bases:** blueprints (block lists) are reliable, persistent replicated data; a ship in flight is a
  replicated rigid body, predicted for its pilot. Players inside a ship replicate positions relative to the ship's
  frame, which keeps numbers small and motion smooth (Space Engineers' hierarchical prediction with dynamic
  parenting is the precedent).

The whole tier split rests on Q12: "regenerated" is only safe for data where a cross-platform difference is either
impossible (integer and engine-provided math) or harmless (visual-only, corrected by the server).

### 2.5 Interest management

| Option | Precedent | Fit |
|---|---|---|
| Distance radius around each player | Star Citizen streams "entities that are in visible range" ([CIG](https://api.star-citizen.wiki/comm-links/18397)) | Breaks across frames: a ship 300 km up and a base below are "near" in one frame and not another |
| Grid cells | Iris `UNetObjectGridFilter` ([docs](https://dev.epicgames.com/documentation/unreal-engine/iris-filtering-in-unreal-engine)); Minecraft view distance in chunks | Good inside one frame |
| **Frame first, then cells, then priority** | Iris filter + prioritizer; [Fiedler's priority accumulator](https://gafferongames.com/post/state_synchronization/) | Recommended |

1. **Frame filter.** A client is interested in its own frame chain (system → body → surface or ship interior) and
   nothing else. A player in another system is a roster line (name, system id) at a low rate.
2. **Cell filter.** Within a frame, entities are bucketed into cells of fixed size in meters (Q1's body-fixed or
   ship frame). Radii are meters per entity class: characters and creatures ~ hundreds of meters, ships in space ~ tens
   of km, celestial nothing (regenerated). Radii never follow s.
3. **Priority under a byte budget.** Each client gets a downstream budget (default 256 kbit/s, Fiedler's target);
   priorities accumulate until sent, so far or idle objects are delayed, never starved.
4. **Server cost** is the union of occupied places, not the universe. Places nobody occupies are frozen and caught
   up on the next visit (Q7). A server-wide cap on simultaneously occupied bodies protects listen hosts.

**Server meshing later.** The filter keys on frames, so "a process per system" is a later change in who owns a
frame's entities. Keep `authority` (which process simulates it) and `NetId` (which never changes) as separate
fields now; build nothing else.

### 2.6 Wire format, transport and handshake

- **Data plane: binary, bit-packed, generated from reflection.** A component is replicated when it carries a
  `Replicated` trait; its members encode by `value_kind`, quantized by the Flecs member range the component already
  can declare (`.range(min, max)`, present in Flecs 4.2 meta). Orientation as smallest-three (29 bits, Fiedler);
  deltas against the last acked baseline. Script components are reflected structs, so Luau-defined state
  replicates through the same path.
- **Positions are scale-free:** (frame id, cell key, offset in cell). Cells are a fixed size in meters in the owning
  frame; cell keys are varints, so real scale costs a few more bytes per key, never a different format. Velocities
  are float32 (orbital speeds reach ~8 km/s at s = 1; a quantization range tied to s would break the "independent of
  scale" rule). s itself is in the manifest, never in a field width.
- **Ids:** `NetId` is a server-issued u64; generated content is addressed by its seed-path id (Q3's 64-bit galaxy
  ids, chunk keys, instance ids). In JSON (control plane, MCP, logs) every 64-bit id is a decimal string.
- **Control plane: JSON.** Handshake, manifest, errors and a `--net-log` decoder that writes each decoded packet as a
  JSON line (principle 1: text everywhere).
- **Handshake.** The client sends protocol version, engine version, the replicated-schema hash (component names,
  member names, kinds, ranges) and the universe manifest (seed, generator versions per layer, mod hashes, scale
  factors; Q11). The server accepts and sends the component table (name → wire index) and the current tick, or
  refuses with `net.manifest_mismatch` naming what differs and how to fix it (errors tell you the fix). No mod
  download for now.
- **Transport:** a library behind a `Transport` seam: [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
  (Valve, BSD-3, reliable and unreliable messages, encryption, NAT traversal and relays) or
  [yojimbo](https://github.com/mas-bandwidth/yojimbo) (Fiedler, BSD-3, client/server, "100 players or fewer").
  Plus an **in-memory transport with seeded latency, jitter and loss counted in ticks**, which makes network tests
  deterministic. The library choice is a spike (dependencies and Windows build unverified).

## 3. Tradeoffs

| | A. Lockstep | B. Predict/rollback | **C. Server state + local prediction** | D. Distributed ownership |
|---|---|---|---|---|
| Complexity | Low code, very high discipline (every system deterministic forever) | Highest: whole-world snapshots and re-simulation | Medium: replication, prediction for few systems, interest | Medium, plus ownership handoff bugs |
| Performance (client) | Simulates the whole world | Simulates the whole world, several times on rollback | Simulates what is near; re-simulates only the local player | Simulates what it owns |
| Bandwidth | Tiny, independent of object count | Tiny | Grows with nearby movers; bounded by the budget | Like C |
| Determinism needed | Bit-exact, everything, cross-platform | Bit-exact, everything | Only edit application (by construction) and best-effort local prediction | Little |
| Extensibility (Luau mods) | A mod with one nondeterministic line desyncs everyone | Same | A mod is server-side truth; clients only need matching generators for regenerated data | Like C |
| Testability | Excellent replay; desyncs hard to find | Hard | Good: headless server and clients, deterministic in-memory transport, replay per process | Hard (ownership races) |
| Scale range | Breaks with players in different systems | Same | Frame filter makes it independent of s and of universe size | Like C |

## 4. Extension points

| What | Luau | C++ module | Toolbox core | Engine |
|---|---|---|---|---|
| Replicate a new component | Script components marked replicated in `veach.component` (a flag) | `.add<Replicated>()` plus member ranges | | The codec |
| Custom requests (interact, trade, build) | Define a request and its server handler; the handler writes through a command buffer | Same in C++ | | Request channel, validation hook |
| Prediction for a new controller (a flight model) | | Mark a system as predicted; it must be a pure function of its inputs and components | | Rollback/replay of predicted systems |
| New edit kind (a new brush, a mining laser) | Op data + parameters | Op applier (integer math) | Edit log per body | Ordered op stream per channel (generic) |
| Interest rules for a new entity class | Radius and priority per class (data) | Custom filter or prioritizer | Frame filter (knows the frame tree) | Filter and prioritizer seams, budget |
| Topology (server meshing, distributed ownership) | | | | Authority field per entity; changing owners is core work later |
| Transport | | Implement `Transport` | | Library + in-memory transport |

Generic pieces go in the engine (`engine/net`: identity, codec, transport, filters, prediction, op streams), per
[ADR 0042](../adr/0042-space-toolbox-layering.md). The toolbox adds frame-aware interest, terrain and flora edit
logs, and the manifest contents.

## 5. Interactions with the other questions

- **Q1 coordinates:** owns the frame tree, cell size and body-fixed frames; wire positions are (frame, cell,
  offset). Moving ship frames decide how interior positions replicate. Luau/JSON id rules are shared.
- **Q2 mechanics and time:** the server tick is the clock; orbits and local time are functions of it. No warp in
  multiplayer; the tick type must be the `uint64` step counter, not `elapsed` in doubles.
- **Q3 pipeline:** stable seed-path ids for systems, bodies, chunks and instances; which stages are
  gameplay-relevant (must be CPU and deterministic) vs visual.
- **Q4 terrain:** integer or fixed-point density for edited chunks; a CPU path for collision near every player on a
  GPU-less server; chunk hashes; voxel snapshots for repair and late join.
- **Q5 procgen graph:** Luau hooks run on server and client, so they must use `@veach` deterministic math; a hook's
  output tier (regenerated vs cosmetic) is declared.
- **Q7 simulation LOD:** the server runs catch-up for places players enter; NPCs, economy and factions are server
  state; system edits (eruptions) use the edit log.
- **Q8 renderer:** nothing networked; interpolated and predicted state reach it through extraction like any
  component. Render interpolation (`FixedTimestep::alpha`, unused today) becomes needed for remote entities.
- **Q9 physics:** the server's physics is authoritative; clients predict only their own bodies, so the physics
  engine need not support whole-world rollback. Moving-frame physics decides ship interiors.
- **Q10 travel:** approach speed bounds how fast a client's interest moves, hence the streaming of replicated state
  and the frame handoffs; FTL is a frame change on the wire.
- **Q11 persistence:** the server owns the world save; the save and the wire share ids, the manifest, the edit logs
  and the reflection codec (binary on the wire, text-first in saves). Recordings must include the manifest.
- **Q12 determinism:** decides which data can be regenerated. Networking's minimum asks: integer edit application,
  engine-provided math for gameplay-relevant generation, GPU output visual-only, and a CI test comparing hashes on
  macOS arm64 and Windows x64.

**Conflict to resolve in the synthesis:** if Q4 picks GPU-only meshing or density generation, a dedicated server
without a GPU can't know where the ground is. Networking requires a CPU path (at least coarse density and colliders
near players), or dedicated servers with GPUs (expensive to host). I recommend the CPU path.

## 6. M1-class vs M3 Pro-class

Numbers here are estimates from Fiedler's figures and our sizes, not measurements; only the M5 Max can be measured.

| Cost | Client (either chip) | Listen host on base M1 (4 P-cores, 8 GB base, Apple spec, unverified) | Listen host on M3 Pro (5-6 P-cores, 18 GB base, unverified) |
|---|---|---|---|
| Encode/decode | Well under 1 ms per frame | Grows with clients × entities in interest | Same, more headroom |
| Prediction | Replay ~10 ticks (150 ms RTT) of one character or ship per correction: negligible | Same | Same |
| Server simulation | — | Its own place plus every remote player's place: CPU terrain colliders, creatures, physics. Three friends on three other planets ≈ four places generating at once | Same work, more cores and memory for chunk caches |
| Downstream bandwidth | ≤ 256 kbit/s budget (default) | — | — |
| Host uplink | — | 4 clients ≈ 1 Mbit/s, 8 ≈ 2 Mbit/s, 32 ≈ 8 Mbit/s at full budget | Same |
| Edits | < 5 kbit/s even while digging continuously | Op logs per body in memory | Same |

The M1 problem is not networking; it is that a listen host renders and also simulates every place its friends
are. Two mitigations, both product-facing: cap listen sessions at a small group (4), and optionally require
co-op players to stay within the host's system (NMS-like "join the host"). Dedicated servers remove the problem.

## 7. Recommendation

1. **Server-authoritative state replication with client prediction** of the local player's movement, controlled
   ship and own edits. Others are interpolated from server state. No lockstep, no rollback of the world.
2. **One world, one role per process.** Authority (single player, listen host, dedicated) or proxy (client).
   Single player runs the authority code path with no connections. A dedicated server is any Veach app run headless on
   the null RHI with a net module.
3. **Three tiers of state** (regenerated, replicated, cosmetic), declared per component and per generator output.
4. **Edits are server-ordered operation logs per body,** applied with integer math, verified by chunk hash, repaired
   by voxel snapshot; the same path for system edits.
5. **Interest by frame, then by meter-sized cells, then priority** under a per-client budget (256 kbit/s default).
6. **Reflection-driven binary codec** with quantization from Flecs member ranges, scale-free positions
   (frame, cell, offset), `NetId` plus seed-path ids, a JSON control plane, a manifest and schema handshake.
7. **Reuse record/replay and MCP for net debugging.** A server recording captures every client's inputs and
   requests per tick (a full server session replays headless on one machine); a client recording captures received
   packets (a client session replays without the server). The per-frame state hash doubles as the prediction check.
   MCP gains `net_status` (peers, RTT, bytes per channel), `net_entities` (what this process knows and who owns it)
   and `net_conditions` (latency, loss), so an agent can drive a server and two clients headless in one test.

**What would change my mind:**

- Q12 shows cheap cross-platform bit-exact determinism for *all* simulation, including Luau and physics, *and* the
  product drops "players in different systems at once" → lockstep becomes attractive for its tiny bandwidth.
- The owner wants hundreds of players in one shared universe → server meshing moves from an extension point to a
  founding decision (replication layer, per-frame authority across processes).
- Listen hosts on M1-class Macs prove unable to hold four places at once → distributed ownership (Valheim-style:
  the nearest player simulates their place) becomes worth its cheating and handoff costs, for co-op only.
- Q4 can't provide a CPU density and collider path → dedicated servers need GPUs, which changes hosting.

## 8. What is expensive to undo

- **Simulation code that doesn't know authority.** If gameplay systems are written assuming they always run and
  always win, retrofitting server authority means touching every one of them (Minecraft rebuilt single player on a
  server for this reason). From the first slice, systems declare authority-only, predicted, or everywhere.
- **Gameplay-relevant data that only the GPU produces.** Colliders and resource placement must have a CPU path.
- **Ids.** Unstable instance ids (generation order, `entity_t`) baked into saves and edit logs can't be fixed later
  without migrating every world.
- **Edit representation.** Float density in edited chunks, or edits stored as voxel blobs only, makes op replay
  nondeterministic or heavy forever. Integer edit storage is cheap now.
- **Positions and quantization tied to s.** Any field width chosen for one scale breaks real scale.
- **The manifest's contents.** What counts as "the same universe" (seed, generator versions, mod hashes, s) is
  checked on every connect and in every save.
- Cheap to change later: the transport library, priorities, budgets, snapshot rates, interpolation delay.

## 9. Proposed ADR titles

1. Networking is server-authoritative state replication with client prediction, not lockstep
2. One world per process with an authority role: single player, listen and dedicated share one code path
3. State tiers: regenerated, replicated, cosmetic
4. Network identity: server-issued `NetId` and seed-path ids for generated content
5. Replication through reflection: replicated components, quantization from member ranges, binary wire, JSON debug log
6. Edits are server-ordered operation logs per body, verified by chunk hash and repaired by snapshot
7. Interest management by frame, cell and priority under a per-client byte budget
8. Connect handshake checks protocol, replicated schema and universe manifest
9. Network transport behind a seam, with a deterministic in-memory transport for tests (library chosen by spike)
10. The server owns the universal clock; no time warp in multiplayer (joint with Q2)

## 10. Spike candidates

None of these block the first slice, which is single player. The first slice should only respect the authority
seam (recommendation 2) and integer edit storage. Spikes, smallest first:

**N1. Replication loop.** Two headless lab processes on the in-memory transport, then on loopback UDP: a server and
a client. One replicated fly-camera entity driven by the client's input, predicted and reconciled; the handshake
with a deliberately wrong mod hash refused with `net.manifest_mismatch`. Scope: `NetId`, a codec for two component
types with member ranges, input upstream, delta snapshots downstream, `net_status` over MCP; no interest
management. *Visible success:* at 150 ms simulated latency and 5% loss, the client's camera moves without
stutter, the gap between predicted and server position stays under 1 cm after corrections, `net_status` shows
bytes per second, and a recording of the client session replays with the same hash every frame.

**N2. Edit log.** Server plus two clients digging into the same toy voxel grid (not real terrain) at once, integer
density, ops ordered by the server, per-chunk hashes; a late joiner receives the log; a client with a corrupted
chunk is healed by snapshot. Run the server on macOS and a client in Windows CI (null RHI). *Visible success:*
identical chunk hashes on all three processes and both platforms; screenshots of the grid match; a corrupted chunk
logs one repair and then matches.

**N3. Interest across frames.** A server with two clients in two different "systems" (frames), each with a few hundred
replicated movers, under a 256 kbit/s budget. *Visible success:* per-client bytes (MCP `net_status`, Tracy plot)
don't change when the other system's entity count grows tenfold; doubling the movers in a client's own cell keeps
it under budget with lower update rates for far movers, none starved.

Size: N1 is a net module skeleton and two components; N2 adds an op stream and a toy grid; N3 adds the frame filter
and priority accumulator. N3 waits for Q1's frame tree; N2 waits for Q4's density representation.

## 11. Product decisions surfaced

| Decision | Recommendation |
|---|---|
| Player count per server | Design for small co-op: 4 on a listen server, 8-16 on a dedicated server, with nothing in the formats that caps it at 32 (NMS Beyond's ceiling, [PC Gamer](https://pcgamer.com/no-mans-sky-beyond-will-add-32-player-support-powered-bases-cooking-and-alien-milking)). Not an MMO |
| Dedicated vs listen | Both, from one code path. Dedicated first (headless, testable in CI), listen for co-op |
| Peer-to-peer | No |
| Can co-op players spread across systems? | Yes on dedicated servers. On listen servers, recommend a creator setting, default "anywhere" with a cap on occupied bodies; measure on M1 before tightening |
| Mac-Windows cross-play | Yes. It is the reason Q12's integer edit path and CI hash test exist |
| Time warp in multiplayer | None; one server clock |
| Mods in multiplayer | Client and server manifests must match exactly; refused with a precise diff. Mod download from the server later, not now |
| World ownership | The server owns the world save and characters on it; portable characters between servers is a later product question |
| PvP and anti-cheat | Co-op first. Server authority covers basic cheating; no competitive PvP guarantees (anti-cheat stays (c)) |
| Shared universe (one galaxy for all players, NMS-style discoveries) | Out for now; a discovery service is a (b) module in 00-scope |

## 12. Sources

Primary or official:

- Factorio, Friday Facts #76 "MP inside out": https://factorio.com/blog/post/fff-76
- Factorio wiki, Desynchronization: https://wiki.factorio.com/Desynchronization; FFF #340: https://forums.factorio.com/82891
- Glenn Fiedler, Deterministic Lockstep: https://gafferongames.com/post/deterministic_lockstep/
- Glenn Fiedler, State Synchronization: https://gafferongames.com/post/state_synchronization/
- Glenn Fiedler, Snapshot Compression: https://gafferongames.com/post/snapshot_compression/
- Glenn Fiedler, Floating Point Determinism: https://gafferongames.com/post/floating_point_determinism/
- Gabriel Gambetta, Fast-Paced Multiplayer: https://gabrielgambetta.com/client-side-prediction-server-reconciliation.html,
  https://gabrielgambetta.com/entity-interpolation.html, https://gabrielgambetta.com/lag-compensation.html
- Photon Quantum docs: https://doc.photonengine.com/quantum, https://doc.photonengine.com/quantum/v3/manual/frames,
  https://doc.photonengine.com/quantum/v3/concepts-and-patterns/mispredictions-and-entity-views
- Unreal Iris: https://dev.epicgames.com/documentation/unreal-engine/introduction-to-iris-in-unreal-engine,
  https://dev.epicgames.com/documentation/unreal-engine/iris-filtering-in-unreal-engine,
  https://dev.epicgames.com/documentation/unreal-engine/iris-prioritization-in-unreal-engine
- CIG, Server Meshing and Persistent Streaming Q&A: https://api.star-citizen.wiki/comm-links/18397
- Marek Rosa, Space Engineers multiplayer overhaul (2018): https://blog.marekrosa.org/2018/07/space-engineers-multiplayer-overhaul/
- Keen VRAGE engine: https://keenswh.com/vrage
- Jan Hlousek, "Predicted Physics-Based Multiplayer in Space Engineers", GDC 2023: https://gdcvault.com/play/1028839/Online-Game-Technology-Summit-Predicted;
  interview: https://80.lv/articles/jan-hlousek-on-setting-up-in-game-physics-optimization/
- Tim Ford, "Overwatch Gameplay Architecture and Netcode", GDC 2017: https://gdcvault.com/play/1024001 (not watched)
- Jared Cone, "It IS Rocket Science! The Physics of Rocket League Detailed", GDC 2018: https://www.gdcvault.com/play/1024972/ (not watched)
- No Man's Sky NEXT: https://www.nomanssky.com/next-update/; Synthesis: https://www.nomanssky.com/synthesis-update
- Minecraft protocol, chunk format: https://minecraft.wiki/w/Java_Edition_protocol/Chunk_format; 1.3 integrated
  server: https://minecraft.wiki/w/Java_Edition_1.3.1/Development_versions
- GameNetworkingSockets: https://github.com/ValveSoftware/GameNetworkingSockets; yojimbo: https://github.com/mas-bandwidth/yojimbo;
  netcode: https://github.com/mas-bandwidth/netcode

Secondary, community or press (treat as unverified detail):

- Edgegap, Overwatch netcode deep dive: https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback
- Edgegap, Valheim backend deep dive: https://edgegap.com/blog/valheim-multiplayer-game-backend-deep-dive
- Rocket League talk summary: https://glasp.co/youtube/ueEmiDM94IE
- NMS Beyond 32 players: https://pcgamer.com/no-mans-sky-beyond-will-add-32-player-support-powered-bases-cooking-and-alien-milking,
  https://wccftech.com/no-mans-sky-beyond-expands-multiplayer-up-to-32-players-adds-animal-husbandry-logic-electricity-and-much-more/,
  https://opencritic.com/news/1881/no-mans-sky-next-generation-brings-32-player-multiplayer-and-a-free-upgrade-to-xbox-series-x-s-ps5-and-pc,
  https://gamesradar.com/no-mans-sky-multiplayer-tips
- NMS terrain edit reports: https://steamcommunity.com/app/275850/discussions/0/3092275748074322669
- Elite Dangerous architecture (forums): https://forums.frontier.co.uk/goto/post?id=796342
- Star Citizen replication layer (community wiki): https://starcitizen.tools/Replication_layer
- Minecraft 1.3 merge (press): https://www.engadget.com/2012/07/05/minecraft-1-3-to-merge-single-and-multiplayer-coming-august-1/

Repository: [ADR 0007](../adr/0007-scripting-luau.md), [ADR 0018](../adr/0018-engine-lifecycle-and-phases.md),
[ADR 0020](../adr/0020-renderer-data-and-extraction.md), [ADR 0026](../adr/0026-reflection-everywhere.md),
[ADR 0042](../adr/0042-space-toolbox-layering.md), [record-replay spec](../specs/record-replay/spec.md),
[MCP spec](../specs/mcp-server/spec.md), `engine/ecs/include/veach/ecs/command_buffer.hpp`,
`engine/ecs/include/veach/ecs/components.hpp`, Flecs 4.2 `include/flecs/addons/meta.h` (member ranges).

Unverified and flagged in the text: NMS's actual topology; Elite's host model; Overwatch and Rocket League details
(secondary summaries); GameNetworkingSockets' dependencies and Windows build; Apple core counts and base memory; all
CPU and bandwidth figures for Veach (estimates until spike N1).
