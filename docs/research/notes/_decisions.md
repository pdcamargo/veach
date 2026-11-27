# Dev lead's cross-question decisions (input to the synthesis and the ADRs)

Written by the dev lead after reading the twelve research summaries. Where a research doc disagrees with this file,
this file wins. Product decisions are NOT decided here; they go to the review packet (last section).

## Resolved conflicts

1. **Multiplayer model.** Server-authoritative replication with client prediction, never lockstep (Q6, Q12 agree).
   So positions inside frames stay doubles (Q1's int64 fallback is not needed); physics is replay-tier, not canonical.
2. **Determinism tiers (Q12) are the contract everything signs.** Canonical (bit-identical Mac arm64 = Windows x64,
   CPU only): seeds, addresses, stage records, orbits from ticks, terrain density at collider/edit resolution,
   gameplay placement, edit application, Luau generator hooks, aggregate sim state. Replay (same binary, same
   platform): fixed-step simulation, physics (Jolt is additionally built cross-platform-deterministic). Visual:
   all GPU work. Data only flows downward. Strict FP flags (-ffp-contract=off, no fast-math; MSVC /fp:precise
   with contraction off) on every CPU target; `veach::dmath` correctly rounded transcendentals; stateless
   counter-based hash RNG (keyed by seed/domain/id/tick or index) for procgen and sim LOD; the existing
   veach::Random stream stays for gameplay code that runs live only.
3. **Terrain edit storage.** Wire and journal = versioned fixed-point brush operations, server-ordered per body
   (Q6). At rest = per LOD0 chunk, sparse *absolute* override samples (int16 distance + u8 material) plus the
   `base_hash` of the generator output they were made on (Q4 representation + Q11 validation). Coarse LODs are
   derived from LOD0 overrides exactly. Simulation edits (eruptions) use the same path, recorded when they happen.
4. **Authoritative terrain and procgen run on the CPU.** CPU field-graph interpreter (Q5) and CPU mesher (Q4) are
   canonical and run headless/null RHI (dedicated servers, CI). GPU evaluation/meshing is visual-only, later. So
   VCH-073 (non-blocking readback) is NOT on the critical path; VCH-071 (indirect draw/dispatch) and VCH-072
   (3D textures) are, for the renderer.
5. **Scale model** (mechanism decided here; defaults are product → packet):
   - `s_p` (body scale): planet/moon/asteroid radii, moon orbit radii around their planet, planetary-band terrain
     features (continents, mountain ranges, ocean basins), river widths from scaled catchments with a meter floor.
   - `s_o` (orbit scale): planet orbits around stars, star radii, binary separations. Default `s_o = s_p` (one knob);
     separating them is an advanced creator setting. Eclipses stay coherent because moons follow `s_p` and stars
     follow `s_o` (Q2 corrects 00-scope §5).
   - Never scaled (meters): players, creatures, props, buildings, ships, caves (cave band depth in meters), edit
     brushes, LOD0 cell size, detail-band terrain, human-scale haze/fog.
   - Atmosphere: thickness and scale heights follow `s_a = sqrt(s_p)` by default (Q8 + Q10 agree: 10 km at
     s = 0.01); scattering/absorption coefficients scale by 1/s_a to keep optical depth (sky color) the same.
   - Gravity: surface g fixed at the physical value (× creator multiplier); gameplay GM = g·R² so GM scales with the
     square of the body's radius scale (s_p² for planets, s_o² for stars). Consequence: orbital speeds and periods
     scale with sqrt(scale); year length follows that rule.
   - Day length: physical sidereal period × creator multiplier, validated: warn when equatorial rotation speed
     > 30% of surface orbital speed, refuse > 70% (Q9/Q10).
   - Relief: planetary-band relief heights scale with s_p^relief_exponent; creator setting, default 0.5 pending
     the eye test in the spike (Q5 S3). Detail band never scales.
   - No stage ever reads s (Q3): records are physical; scale is applied when records become geometry. Stage hashes
     equal across s in CI. Consequence: placed objects (trees) may differ across scales; s is in the universe
     genesis and immutable once anything is anchored (Q1/Q3/Q4/Q11 agree).
   - Travel governor speed cap max(v_floor, d/τ) is in meters and identical at every scale, so travel times are
     scale-independent (Q10). τ = 5 s near bodies, 7 s cruise, v_floor = 100 m/s as starting values, replaced by
     measured chunk budgets (Q4 constants) in the descent spike.
6. **Coordinates.** Doubles relative to a parent frame; frames are entities in a tree (system barycenter → body orbit
   frame → body-fixed rotating frame → base/ship → ship interior). Largest continuous frame = a star system. Galaxy:
   int64 per axis (unit ≤ 1 km), sector + local. No floating origin in simulation. GPU sees only camera-relative
   float32, computed in double at extraction (RenderWorld holds no absolute positions; photo-mode TLAS too). Luau
   gets a double `Position` userdata; `vector` stays float32 for offsets/directions. 64-bit ints cross Luau, JSON and
   MCP as strings/tagged userdata. Surface physics in body-fixed frames without fictitious forces.
7. **Time.** int64 universe tick at 60 Hz; orbital phase exact via integer periods (2^-16 tick units); bodies on rails
   (barycenter tree of registered motion models: Keplerian elliptic/hyperbolic, fixed, scripted); the sky's suns are
   queries of the system (N sun directions), eclipses from an occluder query. Time warp: single player physics warp
   ≤ 4×, rails warp ≤ 100,000× implemented as catch-up; multiplayer: server clock, no free warp.
8. **Renderer (supersedes ADR 0040).** Keep and grow the visibility buffer: GPU meshlet culling + indirect draws,
   material-binned compute shading, clustered light grid, forward pass for transparents. Temporal accumulation at
   native resolution (TAA + temporal reuse for clouds/AO) is allowed: it is anti-aliasing and reuse, not upscaling,
   and does not conflict with ADR 0039 (no real-time GI/path tracing). Shadows: CSM near, analytic planet/eclipse/ring
   shadows far. Ambient: sky SH + GTAO; reflections: SSR + probes. Per-body atmospheres: Hillaire LUTs generalized to
   any planet center/radius/composition, per-pixel ray march from space, N suns summed. Depth: reverse-Z infinite,
   camera-relative, no log depth; distant bodies point → analytic sphere impostor → terrain by screen size. Exposure:
   GPU histogram auto-exposure + pre-exposure (sun disk overflows fp16 otherwise). Quality presets never change
   simulation outcomes. New RHI gaps found: blend state, depth bias, comparison samplers, anisotropic filtering.
9. **Physics.** Jolt (MIT), JPH_DOUBLE_PRECISION + CROSS_PLATFORM_DETERMINISTIC from day one, private to a generic
   `engine/physics`. One physics scene per active frame (body-fixed surface region, ship interior, co-moving free
   ship); tick-scheduled transfers. Gravity from a per-body registry (dominant body by sphere of influence, μ = g·R²),
   artificial gravity volumes override. Terrain colliders CPU-meshed from canonical density at LOD0 near moving
   bodies, installed at tick t+K deterministically. Box3D re-evaluated at its 1.0.
10. **Generation.** Elite-style layered octree 64-bit system addresses with typed sub-addresses (body, surface,
    chunk, object); seeds hashed from (universe seed, address, stage id, stage version); stages typed physical
    records, cached, versioned, with stability classes (frozen / pinned / refreshable); registries by stable string
    id run in id order (body kinds, biomes, phenomena, stages, nodes); procgen = code-first stages + JSON field graphs
    with versioned nodes and a Luau builder; Luau procgen sandbox (per-worker VMs, deterministic math, no world
    access, time budget); galaxy = continuous density + magnitude-layered stars + deep-sky catalog; a universe query
    service exposed to MCP/CLI.
11. **Persistence.** `universe.json`: immutable genesis (UUID, seed string, s_p/s_o/s_a, creator settings, determinism
    contract version) + append-only epochs (stage versions, mods with hashes and roles). Bodies pin to the current
    epoch when first anchored (edit, placed object, override, discovery). Old stage versions stay runnable (C++
    registry with golden hashes; Luau generator mods vendored by hash); bake migration is the only retirement path.
    Save = canonical JSON + binary region files (per-chunk zstd); `veach save dump`. One format for single player and
    servers.
12. **Simulation LOD.** Four tiers per observer (live, reduced, aggregate, dormant) with radii in meters (quality
    presets); fixed sim-cell count per body; state = baseline + persisted deviations + clock functions; catch-up order
    clock function → closed form → event jumps → capped coarse steps → snap to baseline; CI equivalence of live vs
    caught-up; fold-back only of player-caused changes; integer/fixed-point aggregates; clock-aligned staggered coarse
    ticks.
13. **Engine vs toolbox (ADR 0042 "generic goes down").** Engine gains: double positions + frame tree + camera-relative
    extraction; strict FP + dmath + hash RNG; job system (VCH-015, now critical); `engine/physics` (Jolt);
    `engine/procgen` (stage runtime, field-graph IR and interpreters, Luau procgen sandbox); `engine/voxel` (density
    chunks, mesher, edits, streaming); runtime (non-cooked) meshes in the renderer; reflection arrays/int64/entity
    refs; persistent ids; module-registered MCP tools; analog input axes; quality presets. Toolbox (`space/`): galaxy,
    addresses, body kinds, orbits/time model specifics, planetary terrain bands, atmospheres from composition,
    gravity/flight/travel, sim domains, universe manifest semantics.

## Spike plan (backlog ids are fixed here)

Each spike: smallest throwaway prototype, visible success criterion, sized by scope. Ordered toward the first slice.

| Id | Spike | Validates | Success criterion |
|---|---|---|---|
| VCH-077 | SP-01 Strict FP and a cross-platform hash | Q12 tiers, build flags | One committed hash of a canonical computation reproduced by the macOS gate and Windows CI; removing `-ffp-contract=off` on the Mac makes it differ |
| VCH-078 | SP-02 dmath bake-off | Q12 dmath | CORE-MATH vs LLVM-libc vs polynomial: bit-equal results on both platforms over 10^7 inputs; ns/call table in Tracy |
| VCH-079 | SP-03 Precision ladder and frames | Q1 | Double frame tree + camera-relative extraction: imgdiff-clean screenshots at 1 km, 1 AU and 30 AU; Tracy float vs double propagation over 100k entities; Luau `Position` cost within budget |
| VCH-080 | SP-04 Orrery and universe clock | Q2 | Barycenter tree on rails over MCP; Sol preset within JPL approximate positions; orbit hash equal Mac/Windows; sun noon elevations correct at s = 1 and 0.01 |
| VCH-081 | SP-05 System statistics | Q3 | 100k systems headless: histograms, zero orbit-stability violations, stage hashes equal across s and thread counts |
| VCH-082 | SP-06 One field graph, three evaluators | Q5 | CPU interpreter, GPU interpreter and hand-written kernel agree within 1e-5 on Metal and WARP; GPU interpreter within 2× of hand-written; CPU ≥ 10 M samples/s/core |
| VCH-083 | SP-07 Deterministic Luau generator | Q5, Q12 | Climate/biome hook hashes equal on macOS arm64 and Windows x64; fastcall bypass closed; a runaway hook stops with an error code |
| VCH-084 | SP-08 Mesher bake-off | Q4 | Transvoxel vs surface nets: no cracks across LOD seams in screenshots; ms/chunk in Tracy; identical mesh hashes across runs |
| VCH-085 | SP-09 Planet octree and scripted descent | Q4, Q10, Q1 | Descent from orbit with the travel governor at s = 0.01 and s = 1 and onto a 1 km asteroid: no holes in screenshots; peak chunks/s within 20% across scales and within budget in Tracy |
| VCH-086 | SP-10 Atmosphere ground to orbit | Q8, Q2 | Per-body atmosphere from composition, ground → orbit with no seam, at s = 1 and 0.01 (s_a = sqrt(s)); two suns and an eclipse; screenshots + Tracy ms |
| VCH-087 | SP-11 Jolt in Veach | Q9, Q12 | Jolt double + cross-platform deterministic: state hash equal on the macOS gate and Windows CI, with 1 and N workers |
| VCH-088 | SP-12 Dig and collide | Q4, Q9, Q11 | Brush edits replay to identical chunk hashes on arm64/x64; a ball falls into the dug hole; Tracy ms for mesh + cook per chunk |
| VCH-089 | SP-13 Walk, hover and land on a rotating planet | Q1, Q9, Q10 | At s = 0.01 and s = 1: no base drift, |Δv| < 1 mm/s through frame handoffs, landing replays with matching hashes |
| VCH-090 | SP-14 Exposure and depth from cockpit to deep space | Q8 | Auto-exposure sunlit ground → cave → space without clipping; cockpit + gas giant + moon with no depth fighting (screenshots) |
| VCH-091 | SP-15 First slice | all | A seed-generated star system with a rotating, tilted planet seen from orbit, atmosphere from its composition, the correct local time where you are; fly down with no loading screen, land, dig a hole. Same code at s = 0.01 and s = 1, switched by configuration |
| VCH-092 | SP-16 Visibility buffer vs forward on a vegetation field | Q8 | Same scene both ways on the M5 Max: ms in Tracy at 1080p/1440p; after VCH-071 |
| VCH-093 | SP-17 Catch-up equivalence | Q7 | Live vs caught-up hashes equal; a year of 6,144 sim cells caught up in < 1 ms |
| VCH-094 | SP-18 Save identity and body pinning | Q11 | Two-version stage: anchored body unchanged (FLIP 0, same hash) after the update, unanchored body changes; byte-identical save/load/save |
| VCH-095 | SP-19 Replication loop | Q6 | Headless server + client: no visible stutter at 150 ms / 5% loss; manifest mismatch refused with the fix; client recording replays |
| VCH-096 | SP-20 Edit log over the network | Q6, Q4 | Two clients dig; server on macOS, one client in Windows CI: identical chunk hashes; a corrupted chunk healed by snapshot |
| VCH-097 | SP-21 Measure on real M1 and M3 Pro | Q8, ADR 0003 | The SP-09/10/14/16 scenes on real base M1 and M3 Pro machines: Tracy tables replace the estimates (needs hardware: product decision) |

Order to the slice: SP-01, SP-02, SP-03, SP-04, SP-05, SP-06, SP-07, SP-08, SP-09, SP-10, SP-11, SP-12, SP-13,
SP-14 → SP-15. SP-16 when VCH-071 lands. SP-17 to SP-20 do not block the slice. SP-21 as soon as hardware exists.

New engine prerequisite items (generic goes down), ids fixed here:
- VCH-098 RHI: blend state, depth bias, comparison samplers, anisotropic filtering (found by Q8)
- VCH-099 Reflection: arrays, int64 and entity references (JSON, Luau, MCP, replay)
- VCH-100 MCP tools registered by modules
- VCH-101 Analog input axes (mouse and gamepad)
- VCH-102 Runtime (non-cooked) meshes in the renderer
- VCH-103 Persistent entity ids and references
- VCH-015 Job system moves to Now: it blocks procgen, terrain, physics and sim LOD.

## Founding ADRs (numbers fixed here; all Accepted, engineering only)

| # | Title | From |
|---|---|---|
| 0043 | Determinism tiers, strict floating point and veach::dmath | Q12 |
| 0044 | Counter-based hash randomness for generation and simulation | Q12, Q7, Q3 |
| 0045 | Large-world positions: doubles in a reference-frame tree, float32 only on the GPU, no floating origin | Q1 (amends 0013, 0020) |
| 0046 | 64-bit values and positions across Luau, JSON and MCP | Q1, Q3, Q11 |
| 0047 | The universe clock: int64 ticks, exact orbital phase, time-warp policy | Q2 |
| 0048 | Bodies on rails: a barycenter tree of registered motion models; suns and eclipses as queries | Q2 (amends 0038) |
| 0049 | The scale model: body, orbit and atmosphere scale factors; what never scales; derived quantities | cross-question |
| 0050 | Universe addresses and hashed seeds | Q3 |
| 0051 | Generation is a staged pipeline of typed physical records, registered by stable id, that never reads scale | Q3, Q5 |
| 0052 | Procgen runtime: code-first stages and JSON field graphs, CPU-authoritative, GPU visual-only, a Luau procgen sandbox | Q5 |
| 0053 | Terrain is a signed density field in chunked body-local octrees, meshed on the CPU | Q4 |
| 0054 | Terrain edits: brush operations on the wire, absolute override samples at rest; water as levels | Q4, Q6, Q11 |
| 0055 | Universe identity and generator versioning: genesis, epochs, stability classes, body pinning, save layout | Q11 |
| 0056 | Multiplayer: server-authoritative replication, one world per process with an authority role | Q6 |
| 0057 | Simulation LOD: four tiers per observer, baseline plus deltas plus clock functions, verified catch-up | Q7 |
| 0058 | Physics: Jolt in double precision, one scene per frame, per-body gravity, deterministic collider installs | Q9 (amends 0017) |
| 0059 | Gravity, flight and travel: sphere-of-influence gravity, flight-model plug-ins, the travel governor | Q10 |
| 0060 | The real-time renderer: a GPU-driven visibility buffer with clustered lights and forward transparency | Q8 (supersedes 0040) |
| 0061 | Temporal accumulation at native resolution; no upscaling | Q8 |
| 0062 | Shadows, ambient light and reflections without ray tracing | Q8 |
| 0063 | Astronomical rendering: per-body atmospheres, N suns, distant bodies, starfield, depth and exposure | Q8 (supersedes 0038's scope) |
| 0064 | Quality presets never change simulation outcomes | Q7, Q8, Q10 |
| 0065 | Engine areas for the toolbox: frames, physics, procgen, voxel; what stays in space/ | all (applies 0042) |

## Product decisions for the review packet (owner decides; my recommendation)

1. Hardware target: design target M3 Pro-class 1440p60 with everything on; floor = base M1-class 1080p60 on the low
   preset. Buy a used base M1 and an M3 Pro-class Mac to measure (SP-21).
2. Default scale and range: default s = 0.1 (KSP-like: an Earth-like planet ≈ 637 km radius); supported 0.01-1;
   one knob by default (s_o = s_p), separate orbit scale as an advanced setting; fixed once anything is anchored.
3. Creator settings and defaults: surface gravity ×1 (physical); day length ×1 (physical, validated); year length
   follows gravity (√s); atmosphere thickness √s; relief exponent 0.5 (confirmed by eye in a spike); habitability
   bias generous (NMS-like); human-scale haze on.
4. Multiplayer scope: co-op, Mac-Windows cross-play; 4 players on a listen server, 8-16 on dedicated, no format cap;
   dedicated first; mods must match exactly.
5. Realism vs gameplay: fixed surface g; orbital periods follow √s (not real at compressed scale); equal travel
   times at every scale (governor); no time warp in multiplayer; no Coriolis/centrifugal; mild entry heating; FTL
   jumps between systems (no continuous interstellar flight).
6. First slice: in = one seeded system (star, planet, a moon), rotating tilted planet, composition atmosphere, local
   time, descent without loading, landing, digging, s = 0.01 and 1, single player. Out = multiplayer, fauna/flora,
   oceans, clouds, vegetation, ship interiors, sim LOD, galaxy beyond a starfield.
7. Galaxy: Milky Way-sized default (100,000 ly), creator-adjustable.
