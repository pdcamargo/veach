# Q6 networking: raw notes

Research notes for [06-networking.md](../06-networking.md). Date: 2026-11-27. Everything with a link was read
(fetched or seen in search excerpts) during this session; "(unverified)" means it was not checked against a primary
source. Repo facts were read from the tree at commit c520356.

## Authority models and their precedents

### Takeaway
Every shipped game with a big, mostly procedural world and many objects uses server-authoritative state replication
with client prediction, not deterministic lockstep. Lockstep (Factorio) works only when every peer simulates the
whole world and determinism is total, both of which Veach cannot have once players are in different systems and
run user Luau.

### Cited Findings
- Factorio is deterministic lockstep: "each of the networked peers is running the simulation of everything that
  happens in the world"; only player actions cross the network; local actions are delayed by a latency buffer; "the
  game speed is limited by the slowest player"; the 2015 design is "completely peer to peer" with no central server
  required — [FFF #76](https://factorio.com/blog/post/fff-76)
- Factorio desyncs are "a bug in the core Factorio game code or ... a bug in a mod/scenario"; mods "if written
  incorrectly can cause desyncs very easily"; a desync report holds both the client's and server's save — [Factorio
  wiki, Desynchronization](https://wiki.factorio.com/Desynchronization) (search excerpt);
  [FFF #340 Deep desyncs](https://forums.factorio.com/82891) (title only)
- Lockstep sends inputs only, so bandwidth is independent of object count ("a physics simulation of one million
  objects with the same bandwidth as just one"); a playout delay buffer (~100 ms in the demo); redundant unacked
  inputs over UDP survive 2 s latency and 25% loss; floating-point determinism across compilers, OSes and
  instruction sets is "almost impossible to guarantee" — [Fiedler, Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/)
- State synchronization: send inputs and a prioritized subset of object state (position, orientation, velocities),
  extrapolate the rest; does not need exact determinism; priority accumulator; 256 kbit/s target adjustable at
  runtime; a 4-5 frame jitter buffer at 60 Hz; quantize on both sides (4096 values per meter, 15 bits per quaternion
  component) — [Fiedler, State Synchronization](https://gafferongames.com/post/state_synchronization/)
- Snapshot compression of 901 cubes at 60 Hz starts at 17.37 Mbit/s; smallest-three quaternions (29 bits), quantized
  positions, delta against the last acked baseline (1 bit per unchanged cube), indices of changed cubes; the target
  is 256 kbit/s — [Fiedler, Snapshot Compression](https://gafferongames.com/post/snapshot_compression/)
- Client prediction and server reconciliation for the local player, entity interpolation (others shown slightly in
  the past from real server data), lag compensation — [Gambetta Part II](https://gabrielgambetta.com/client-side-prediction-server-reconciliation.html),
  [Part III](https://gabrielgambetta.com/entity-interpolation.html), [Part IV](https://gabrielgambetta.com/lag-compensation.html)
  (search excerpts)
- Overwatch (Tim Ford, GDC 2017): fixed 16 ms command frames; the client runs ahead of the server by RTT/2 plus a
  buffered frame; movement, abilities and weapons are predicted; on misprediction the client rolls back to the
  server's movement state and replays buffered inputs; server-side input buffer refilled by client time dilation
  (~15.2 ms steps); clients send a sliding window of unacked inputs; only three of ~46 client systems handle
  gameplay netcode — [Edgegap summary of the talk](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback)
  (secondary); talk listing [GDC Vault 1024001](https://gdcvault.com/play/1024001)
- Rocket League (Jared Cone, GDC 2018) networks physics; a third-party summary says physics runs at 120 Hz with a
  fixed step for consistency — [GDC Vault](https://www.gdcvault.com/play/1024972/); summary
  [Glasp](https://glasp.co/youtube/ueEmiDM94IE) (secondary, unverified)
- Photon Quantum: a deterministic ECS with predict/rollback; Q48.16 fixed-point `FP` replaces floats; "verified"
  frames are identical across clients, "predicted" frames can differ; rollbacks show as view jumps, smoothed or
  reduced with input delay — [Quantum intro](https://doc.photonengine.com/quantum),
  [frames](https://doc.photonengine.com/quantum/v3/manual/frames),
  [mispredictions](https://doc.photonengine.com/quantum/v3/concepts-and-patterns/mispredictions-and-entity-views)
- Space Engineers: VRAGE has "client-server architecture, support for both dedicated servers and in-game server,
  streaming technology to replicate parts of the world on client" — [Keen VRAGE page](https://keenswh.com/vrage)
  (search excerpt); the 2018 overhaul targeted 1.0 sim speed with 16 players and 100,000 PCU per world, saw "32
  player servers with heavy activity" at 1.0; official server hardware i7-6700K, 32 GB, 250 Mbps, ~3 cores and 6 GB
  per instance; optimizations: lower rate for inactive entities, update priority, frequency adapted to server load —
  [Rosa 2018](https://blog.marekrosa.org/2018/07/space-engineers-multiplayer-overhaul/)
- Space Engineers' prediction is "hierarchical prediction" with dynamic parenting (GDC 2023, Jan Hlousek) —
  [GDC Vault](https://gdcvault.com/play/1028839/Online-Game-Technology-Summit-Predicted),
  [80.lv interview](https://80.lv/articles/jan-hlousek-on-setting-up-in-game-physics-optimization/) (abstract cut off)
- Valheim: each object (ZDO) is simulated by one peer and relayed through the host; the first player in an area
  becomes its owner and keeps it until they leave; Iron Gate says it is a misconception that Valheim is P2P; 1-10
  players; a changed ZDO is re-sent whole — [Edgegap Valheim](https://edgegap.com/blog/valheim-multiplayer-game-backend-deep-dive)
  and hosting guides via search (community analysis, partly unverified)
- Elite Dangerous: servers do matchmaking and set up peer-to-peer "islands"; instance state is P2P; players report
  poor connectivity in crowded instances; whether one peer is the instance host is disputed — Frontier forum threads,
  e.g. [Systems Architecture](https://forums.frontier.co.uk/goto/post?id=796342) (community, unverified)
- Star Citizen: clients connect to a Gateway, which forwards to Replicants (the replication layer), which run "very
  little code"; one server node has authority over an entity, other nodes have "a client view of this entity";
  entity state persists in a graph database ("EntityGraph"); static meshing fixes node regions in advance, dynamic
  meshing moves authority with load; "The client already only streams entities that are in visible range"; shard
  count not committed ("many small shards per region") — [CIG Server Meshing Q&A](https://api.star-citizen.wiki/comm-links/18397);
  community wiki [Replication layer](https://starcitizen.tools/Replication_layer); 500 players per shard at the
  4.0 preview (community wiki, unverified)
- No Man's Sky: Beyond (2019) raised sessions to 32 players on PC, 16 on consoles at launch; next-gen update 3.10
  (2020) brought 32 to next-gen consoles; no traditional host — [PC Gamer](https://pcgamer.com/no-mans-sky-beyond-will-add-32-player-support-powered-bases-cooking-and-alien-milking),
  [Wccftech](https://wccftech.com/no-mans-sky-beyond-expands-multiplayer-up-to-32-players-adds-animal-husbandry-logic-electricity-and-much-more/),
  [OpenCritic](https://opencritic.com/news/1881/no-mans-sky-next-generation-brings-32-player-multiplayer-and-a-free-upgrade-to-xbox-series-x-s-ps5-and-pc),
  [GamesRadar](https://gamesradar.com/no-mans-sky-multiplayer-tips) (press; console caps disagree between sources)
- NMS NEXT (2018): "Team up with a small group of friends ... or be joined by random travellers"; no session cap on
  the official page (the brief's "up to 32" applies to Beyond, not NEXT) — [nomanssky.com NEXT](https://www.nomanssky.com/next-update/)
- Minecraft 1.3 (snapshot 12w18a, 2012): single player runs on an internal local server, splitting render and world
  threads; Mojang noted it raised resource needs — [Minecraft wiki](https://minecraft.wiki/w/Java_Edition_1.3.1/Development_versions),
  [Engadget](https://www.engadget.com/2012/07/05/minecraft-1-3-to-merge-single-and-multiplayer-coming-august-1/)
- Unreal Iris: opt-in replication system; replication is push-based ("expects to be notified whenever a replicated
  property is modified"); filtering decides which connections an object replicates to (owner, connection, group,
  dynamic filters; a grid filter for cells near the viewer); prioritizers assign float priorities, below 1.0 means
  skipped this frame — [Iris intro](https://dev.epicgames.com/documentation/unreal-engine/introduction-to-iris-in-unreal-engine),
  [filtering](https://dev.epicgames.com/documentation/unreal-engine/iris-filtering-in-unreal-engine),
  [prioritization](https://dev.epicgames.com/documentation/unreal-engine/iris-prioritization-in-unreal-engine)

### Inferences
- Lockstep's two preconditions both fail for Veach: (1) every peer simulates the whole world, but players may be in
  different star systems, so every client would simulate every occupied planet; (2) bit-exact determinism across
  macOS arm64 and Windows x64, Luau mods and the physics engine. Factorio, the best lockstep game there is, still
  fights mod desyncs.
- Rollback netcode (Quantum, GGPO) needs a whole-world deterministic re-simulation; with streamed terrain and
  physics, rolling back the world is not affordable. Overwatch-style prediction of only the local player's movement,
  ship and own edits is.
- Valheim's "first peer in the zone simulates it" is a cheap way to offload a host but makes cheating and laggy
  owners everyone's problem; it is an extension, not a base.
- Star Citizen's architecture separates "who simulates an entity" (authority, per entity) from "who stores it"
  (replication layer, graph DB). Veach should keep those two fields separate in its design now, even with one
  process doing both.

### Gaps
- No primary source on NMS's actual networking topology (who simulates a planet when four players are on it). Press
  says "no traditional host"; everything else is guesswork.
- Elite's topology is only documented by players.
- The Overwatch numbers are from a secondary summary; the GDC video was not watched.
- Valve's Source networking page (tick, interpolation 100 ms) returned 403; not used.

## What is regenerated vs synced

### Takeaway
Send what players changed and what moves; regenerate what the seed and clock decide. NMS shows the failure mode:
edit storage with a fixed budget and edits from other players' bases flooding a client's save.

### Cited Findings
- NMS Synthesis (2.2, 2019): terrain edits inside a base "are now protected from regeneration"; patch 2.16 blocks
  edits in bases you lack permission for or within 2 km of a featured base; a 2021 fix addressed protected base
  edits being overwritten "when downloading a large number of terrain edits from other player's bases" — patch notes
  via [nomanssky.com Synthesis](https://www.nomanssky.com/synthesis-update) and search excerpts
- NMS players report that edits from other players' bases fill the limited edit buffers and terrain "regrows" past
  the limit — [Steam discussions](https://steamcommunity.com/app/275850/discussions/0/3092275748074322669) (community)
- Minecraft sends whole chunk sections to clients: per section a block count, a paletted container of 4096 block
  states (palette sizes chosen per section), biomes in 4×4×4 regions, heightmaps; the client keeps heightmaps updated
  from block changes itself — [Minecraft wiki, Chunk format](https://minecraft.wiki/w/Java_Edition_protocol/Chunk_format)
- Star Citizen persists every networked entity's state in the EntityGraph and can reseed a new replication layer
  from it — [CIG Q&A](https://api.star-citizen.wiki/comm-links/18397)
- Valheim splits world data into chunks for saving (1.0 patch notes, quoted by Edgegap) —
  [Edgegap](https://edgegap.com/blog/valheim-multiplayer-game-backend-deep-dive)

### Inferences
- Edits should cross the wire as operations (brush kind, center, radius, strength, material, tick, author), tens of
  bytes each, ordered by the server per body. Voxel snapshots are the repair and late-join path, not the normal
  path.
- Applying the same op log on two machines only gives the same voxels if the brush application is bit-deterministic
  (Q12). Integer or fixed-point density makes that cheap to guarantee; a per-chunk hash catches the rest, and the
  server repairs a mismatching chunk with a snapshot. Divergent base terrain (float noise across ARM and x64) is
  harmless for visuals; it matters only where clients predict collisions, and there the server corrects them.
- Creatures: species, look and spawn tables are regenerated; individuals that can interact with players are server
  state; distant ambient life can be client-only cosmetic.
- Orbits, rotation, day/year: a function of the server tick (Q2), so zero bytes after the clock is synced.

### Gaps
- No source found on how Space Engineers synchronizes voxel edits (ops vs voxel data); the GDC 2023 abstract is
  about physics prediction only.

## Interest management across systems

### Takeaway
Filter by frame first (which system, which body), by meters second (cells in the body-fixed or ship frame), then
prioritize under a per-client byte budget. Server meshing (one process per region) is an extension of the same
filter, not something to build now.

### Cited Findings
- Iris: per-connection filtering (owner, group, dynamic, grid) then prioritization under bandwidth limits —
  [Iris filtering](https://dev.epicgames.com/documentation/unreal-engine/iris-filtering-in-unreal-engine)
- Fiedler's priority accumulator: priorities accumulate until sent, so low-priority objects are delayed, never
  starved — [State Synchronization](https://gafferongames.com/post/state_synchronization/)
- Star Citizen: clients stream entities in visible range; meshing decouples client and server streaming radii;
  authority per entity can move between nodes — [CIG Q&A](https://api.star-citizen.wiki/comm-links/18397)
- Space Engineers: sending data less often for inactive entities, frequency adapted to load —
  [Rosa 2018](https://blog.marekrosa.org/2018/07/space-engineers-multiplayer-overhaul/)

### Inferences
- In a universe of seeded systems, a client in system A needs nothing from system B except a roster line. The
  server's cost is the union of occupied places, not the size of the universe.
- The interest radius is meters and never follows s; the frame filter makes it scale-free.

### Gaps
- No public numbers on how NMS or Elite size interest radii.

## Fit with Veach

### Takeaway
Veach already has most of the hard parts: fixed 60 Hz steps with a uint64 step counter, reflection over every
component, transactional command buffers, a per-frame state hash, record/replay and headless MCP. What it lacks is
stable network identity, binary serialization, change detection outside command buffers, and arrays in reflection.

### Cited Findings (repository)
- Fixed 1/60 s simulation phases; headless runs one step per frame; windowed accumulates up to 8 steps per frame;
  `Random` seeded from `--seed`; no wall clock in simulation — `docs/adr/0018-engine-lifecycle-and-phases.md`
- `Time` holds `frame`, `fixed_step` (uint64, steps completed), `delta`, `elapsed` (double) —
  `engine/ecs/include/veach/ecs/components.hpp`
- Record/replay: JSON Lines header (seed, script, fixed step) plus per frame `{frame, steps, events, state}`; the state
  hash is FNV-1a over Scriptable components in entity order, with per-component hashes to name the divergence; MCP
  edits are recorded and replayed — `docs/specs/record-replay/spec.md`
- One reflection walker feeds JSON, schemas, Luau and MCP; `unsupported` kinds (arrays, vectors, maps) are skipped —
  `docs/adr/0026-reflection-everywhere.md`
- `CommandBuffer` holds `Spawn{entity, name, parent}`, `Despawn`, `Set{entity, component, value}`, `Remove`, all with
  world-local `ecs::entity_t` ids; `spawn` reserves a local id immediately — `engine/ecs/include/veach/ecs/command_buffer.hpp`
- Scripts write only through command buffers; reads see the world at invocation start — `docs/adr/0007-scripting-luau.md`
- MCP: headless frames advance only through `step_frames`; writes are transactional; no auth because stdio —
  `docs/specs/mcp-server/spec.md`; MCP over HTTP is VCH-048
- Flecs v4.2.0 meta supports per-member ranges: `EcsMemberRanges {value, warning, error}` and C++ `.range(min, max)`,
  `.warning_range`, `.error_range` — `.cpm-cache/flecs/dcf0/include/flecs/addons/meta.h` lines 214-261,
  `addons/cpp/mixins/meta/untyped_component.inl` lines 123-128
- Renderer reads the ECS only in Extract — `docs/adr/0020-renderer-data-and-extraction.md`

### Inferences
- Entity ids and component ids are local to a world; neither can go on the wire. Need a `NetId` (u64, server-issued)
  and a component table exchanged at connect (reflected name → wire index, plus a schema hash).
- Command buffers are the right unit for *client requests* and for the server's change log, but C++ systems that
  write with `get_mut`/`set` bypass them, so replication needs Flecs change detection or OnSet observers as well.
- Flecs member ranges can carry quantization bounds without a second annotation system.
- A dedicated server is the lab (or a game) with `--headless --rhi null --net-listen`; nothing gameplay needs may come
  from the GPU on a server.

### Gaps
- Flecs `.range()` semantics for serialization were read from the header only; whether ranges are inherited by
  nested struct members was not checked.

## Wire format, transport and handshake

### Takeaway
Binary, bit-packed, reflection-driven data plane; JSON control plane and a JSON debug decoder; a handshake that
refuses mismatched manifests with the fix. Transport from a library behind a seam, plus an in-memory deterministic
transport for tests.

### Cited Findings
- GameNetworkingSockets: Valve, BSD-3; message-oriented connections with reliable and unreliable messages,
  fragmentation, encryption, P2P and NAT traversal; a subset of Steamworks networking that does not need Steam —
  [GitHub (mirror readme)](https://github.com/michicc/GameNetworkingSockets); upstream ValveSoftware/GameNetworkingSockets
- yojimbo: Fiedler, BSD-3, client/server games "optimized for 100 players or fewer", connect tokens, encrypted and
  signed packets, reliable-ordered and unreliable-unordered messages, built on netcode and reliable —
  [yojimbo](https://github.com/mas-bandwidth/yojimbo) (search excerpt)
- netcode.io is a connection layer without reliability — [netcode](https://github.com/mas-bandwidth/netcode) (search excerpt)

### Inferences
- Positions on the wire as (frame id, cell key, offset in cell); cells of a fixed size in meters in the owning
  frame (body-fixed, ship, system). Cell keys are varints, so real scale only costs a few bytes more, never a format
  change.
- Ids as decimal strings in JSON (00-scope: JavaScript loses integers above 2^53).

### Gaps
- GameNetworkingSockets' dependency set (protobuf, OpenSSL or libsodium) and Windows build were not checked
  (unverified); a spike decides the library.

## Costs on M1-class vs M3 Pro-class clients

### Takeaway
Networking itself is cheap on any Apple silicon client. The expensive part is hosting: a listen-server host must
generate collision terrain and simulate around every remote player, on CPU, beside its own rendering.

### Cited Findings
- Fiedler's budget: 256 kbit/s per client is a reasonable target for a physics-heavy scene; stationary steady state
  near 15 kbit/s with delta compression — [Snapshot Compression](https://gafferongames.com/post/snapshot_compression/)
- Space Engineers official servers: ~3 CPU cores and 6 GB per 16-32 player instance, 250 Mbps uplink —
  [Rosa 2018](https://blog.marekrosa.org/2018/07/space-engineers-multiplayer-overhaul/)
- Minecraft's integrated server raised resource requirements for single player —
  [Minecraft wiki](https://minecraft.wiki/w/Java_Edition_1.3.1/Development_versions)

### Inferences (estimates, not measured)
- A replicated moving entity at ~120-160 bits (position in cell, smallest-three orientation, velocity) at 20 Hz:
  100 visible movers ≈ 240-320 kbit/s before delta; well under that after delta and priority.
- Edits: an op is ~40-60 bytes; a player digging continuously at 10 ops/s is under 5 kbit/s.
- Client CPU: encoding/decoding and replaying ~10 predicted ticks of one character or ship per correction is far
  below 1 ms per frame on either chip (estimate).
- Hosting on an M1 (4 performance cores, 8 GB base memory, per Apple specs, unverified here) with three friends on
  three other planets means generating terrain colliders at four places at once; on an M3 Pro (5-6 performance
  cores, 18 GB base, unverified) there is more headroom. Measure in a spike; the host's uplink (4-8 clients at
  256 kbit/s = 1-2 Mbit/s) is the other limit.

### Gaps
- No measured CPU numbers for Veach networking; none can exist before a spike. Only the M5 Max can be measured.
