# 10: Gravity, flight and travel

Added by the [scope gap analysis](00-scope.md#questions-added-or-split). It owns how things fall, how ships fly, and
how fast anything may approach a surface, which is the number terrain streaming (Q4), frame handoffs (Q1) and
rendering handoffs (Q8) are sized against. Raw notes, every source and the arithmetic:
[notes/10-gravity-flight-travel-notes.md](notes/10-gravity-flight-travel-notes.md). Items marked *(unverified)*
could not be checked against a primary source this session.

**Notation.** s scales planet and system lengths (00-scope's s_p and s_sys; this document assumes one s, and nothing
here breaks if Q1/Q2 split them). R is a body's radius at s (Earth-like: R = 6,371 km × s). h is altitude above the
local surface. g_s is surface gravity. τ ("tau") is a time constant in seconds.

## 1. The problem in Veach's terms

The first slice is: see a rotating, tilted planet from orbit, fly down with no loading screen, land, dig, at s = 0.01
and s = 1 with the same code. Today none of the pieces exist:

- **"Up" is a constant.** `world_up` is +Y ([ADR 0013](../adr/0013-conventions-and-math.md)) and the sky reads
  altitude as world Y. On a planet, up is a function of position, and a character walking over a pole must not flip.
- **There is no gravity and no physics engine yet** (Q9 picks Box3D or Jolt). Whatever engine Q9 picks, gravity
  per body is ours to compute: no physics engine knows about patched conics or creator-chosen surface gravity.
- **There is no flight model**, and the input layer only has digital actions (`axis()` returns −1, 0 or 1; mouse
  motion and gamepads are out of scope in the [input spec](../specs/input/spec.md), VCH-036). A flyable ship needs
  analog axes.
- **Scale breaks naive physics.** With surface gravity fixed at 9.81 m/s² (the KSP precedent the brief proposes),
  low-orbit speed is √(g·R): 791 m/s at s = 0.01, 2.5 km/s at s = 0.1, 7.9 km/s at s = 1. Travel distances grow
  100× from s = 0.01 to s = 1. A flight model or travel drive tuned for one end is unplayable at the other.
- **No loading screen means streaming must keep up with the ship.** Simulation is a fixed 60 Hz step that never
  pauses ([ADR 0018](../adr/0018-engine-lifecycle-and-phases.md)); Starfield's approach (stop the tick, tear space
  down, build the surface) is not available. So the fastest a ship may approach a surface is a hard input to Q4.
- **Everything must be deterministic and headless-testable**: a descent from orbit must be recordable with
  `--record` and replay bit-identically, and drivable over MCP.

## 2. Options, with precedents

### 2.1 Gravity model

| Option | What it is | Precedent |
|---|---|---|
| G1 Physical Newton, all bodies summed | g = Σ GMᵢ/rᵢ², masses from the generator | Real physics; SpaceEngine-style tools |
| G2 Physical shape, gameplay strength | Inverse-square falloff from the SOI primary only, with surface g chosen per body and fixed across s | KSP: ~1 g on a 600 km Kerbin *(unverified)*; patched conics |
| G3 Authored curves | Linear or steep falloff chosen for feel; spheres of influence and "alignment fields" | Outer Wilds: linear for planets, inverse-square only for the sun ([PC Gamer](https://www.pcgamer.com/how-to-build-a-black-hole/)); Space Engineers: g × (MaxR/r)^7 ([SE wiki](https://spaceengineerswiki.com/Planet)) |

G1 is wrong in a subtle way once orbits are on rails (Q2): a planet falls freely around its star, so in the planet's
frame the star's pull cancels except for tides. Summing the star's 6 mm/s² into surface physics pulls everything
sideways. Any option must apply only the **SOI primary** in a body's frame. G1 also scales badly: with real masses and
scaled radii, g ∝ 1/s² (981 m/s² at s = 0.01); with real density, g ∝ s (0.1 m/s² at s = 0.01).

**Small irregular bodies.** A 500 m asteroid at 2,000 kg/m³ has g ≈ 2.8×10⁻⁴ m/s² *(computed)*. Physically, you
can't walk on it. The direction near an elongated body should follow its shape (the gradient of the coarse SDF Q4
keeps), blending to radial farther out; the magnitude needs a creator floor or a "mag boots" mode.

**Artificial gravity** (ship decks, bases, spin habitats). Space Engineers sums artificial and natural gravity
*(unverified)* and exposes gravity providers through a registry in its mod API
([IMyGravityProviderSystem](https://keensoftwarehouse.github.io/SpaceEngineersModAPI/api/VRage.Game.ModAPI.IMyGravityProviderSystem.html)).
Summing gives tilted floors in a ship parked on a slope. Overriding inside a volume, with a short blend at the edge,
is the gameplay answer.

### 2.2 Character on a sphere

- **Up from gravity:** up = −ĝ. Rotate the capsule toward it by the minimal rotation each step, with a time constant
  (~0.15 s); below a threshold |g| (e.g. 0.05 m/s²) stop aligning and go 6-DOF. Outer Wilds does this with authored
  alignment fields ([PC Gamer](https://www.pcgamer.com/how-to-build-a-black-hole/)).
- **Heading:** parallel-transport the forward vector with the same minimal rotation. Never derive yaw from north: it
  is undefined at the poles and makes the camera spin as the player walks over one. The compass is a UI readout.
- **Camera up** is smoothed separately from the capsule, so stepping into a ship's gravity volume doesn't snap the view.
- **Controller:** a kinematic capsule with an arbitrary up vector (slope limit, step-up and ground snapping all
  relative to local up). Jolt's `CharacterVirtual` is body-less and updated explicitly after each step
  ([Jolt JNI docs](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/character.html)); its arbitrary up
  vector is *(unverified)*. Q9 decides the engine.
- **Rotating planets:** walk in the **body-fixed frame** (Q1/Q9), where the ground is static. Fictitious forces are
  negligible at physical day lengths: centrifugal ω²R is 0.0003 / 0.003 / 0.034 m/s² at s = 0.01 / 0.1 / 1 with a
  24 h day *(computed)*. They are **not** negligible for short days: a 30-minute day gives 8% g at s = 0.01, 79% g
  at s = 0.1, and more than g (the planet would fly apart) at s = 1. Day length must be validated against the surface
  orbital period 2π√(R/g) = 8.4 / 26.7 / 84.4 min.

### 2.3 Flight model

| Option | What it is | Precedent | At s = 0.01 | At s = 1 |
|---|---|---|---|---|
| F1 Pure Newtonian, orbital | Thrust and gravity only; orbits emerge; maneuver planning | KSP; Outer Wilds (Newtonian with a match-velocity autopilot) | Orbit at 791 m/s: fine | Orbit at 7.9 km/s, Δv ~9 km/s to orbit: needs time warp and planning tools |
| F2 Newtonian with flight assist | Physics forces, but a controller turns stick input into target velocity and rates within thruster authority; assist counters gravity (hover); assist-off is F1 | Elite Dangerous (~300 m/s normal flight, [Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe)); Star Citizen IFCS *(unverified)* | Fine | Fine, because nobody needs orbital speed: leaving a body is cruise's job |
| F3 Arcade | Velocity set directly, gravity mostly ignored in flight, low speed caps in atmosphere | NMS: ~70 u/s average, 190 u/s boost in atmosphere ([NMS wiki](https://nomanssky.gamepedia.com/Pulsing_Heart), search summary; unit unverified) | Fine | Fine |

F2's assist needs a reference frame for "at rest": the body-fixed frame near a body (so hovering and landing work on
a rotating planet), the inertial body-centred frame higher up, the station's frame when docking.

### 2.4 Travel tiers

| Tier | Precedent | What it is |
|---|---|---|
| Local flight | Elite normal flight, NMS thrusters, X4 normal engines | The flight model, colliding, ~100–400 m/s |
| In-system cruise | Elite supercruise (≤ 2001c, capped by proximity and mass of nearby bodies, [Elite wiki](https://elite-dangerous.fandom.com/wiki/Supercruise), [Frontier forums](https://forums.frontier.co.uk/threads/shouldnt-gravity-wells-you-know-like-actually-work.346577/page-2)); NMS pulse (20,000–40,000 u/s, auto-stops near planets); Star Citizen quantum travel (0.18c–0.94c, accelerate/cruise/decelerate, ends at the upper atmosphere, [starcitizen.tools](https://starcitizen.tools/Quantum_travel)); X4 travel drive (~3 s charge, highways, gates, [Egosoft](https://wiki.egosoft.com/X4%20Foundations%20Wiki/Manual%20and%20Guides/X4%3A%20Foundations%20Manual/Piloting%20And%20Travel/)) | A non-Newtonian drive whose cap is a function of where you are |
| Descent / glide | Elite orbital cruise (minimum ~25 km) and orbital glide (forced ~2.5 km/s per players), with emergency drops when too fast at a transition height ([Elite wiki](https://elite-dangerous.fandom.com/wiki/Orbital_Cruise), [Frontier guide](https://community.elitedangerous.com/node/348), search summaries) | A controlled hand-down from cruise to atmospheric flight |
| Interstellar jump | Elite hyperspace tunnel; NMS warp | A transition of bounded length that hides generating the next system |
| Time warp | KSP: on-rails warp, limited by altitude (≤ 4× below 70 km at Kerbin, [KSP wiki](https://wiki.kerbalspaceprogram.com/wiki/Physics_warp)) | Speeds up the universal clock instead of the ship |

**The cap law is the real choice inside cruise.** Three candidates:

- **C1 Fixed speeds per tier** (X4-like). Travel time grows linearly with s: a trip that takes 1 minute at s = 0.01
  takes 100 at s = 1. Fails the scale range.
- **C2 Gravity-weighted** (Elite's "maximum divided by field strength", per a forum paraphrase). Works, but ties
  travel to gameplay gravity, which is a creator setting; changing g would change travel times.
- **C3 Distance-proportional:** |v| ≤ d/τ, d = distance to the nearest significant surface. This is what Elite
  players fly anyway (the "seven-second rule", *unverified*). Time from d₀ to d₁ is τ·ln(d₀/d₁): logarithmic, so
  when every length scales by s, every trip takes **exactly the same time at every s**.

### 2.5 Atmospheric entry

The atmosphere record Q3 generates and Q8 renders is also the drag source: ρ(h) = ρ₀·e^(−h/H). Two models of the
consequences:

- **E1 Physical.** Drag ½ρv²C_dA, heating by Sutton–Graves (q ∝ √ρ·v³). Energy per kilogram ∝ v² ∝ s, so at
  s = 0.01 entry carries 1% of Earth's energy per kilogram: no plasma, no danger. Peak deceleration (Allen–Eggers,
  2° entry) is 0.48 / 1.5 / 4.8 g at s = 0.01 / 0.1 / 1 if the scale height follows √s *(computed)*.
- **E2 Gameplay-relative.** Same drag, but heating and its VFX are driven by v / v_orb(local) and ρ/ρ₀, so entering
  at orbital speed looks and feels the same at every s. KSP's tuned reentry heating is the precedent *(unverified)*;
  NMS's heat effect is reportedly only visual *(forum claim)*.

### 2.6 Transitions without loading

Starfield stops the game tick behind the landing screen to tear space down and build the surface (a modder's
account, [GamesRadar](https://www.gamesradar.com/games/rpg/seamless-landing-mod-looks-set-to-bring-starfield-another-step-closer-to-no-mans-sky/));
Todd Howard called seamless landing "not that important to the player"
([PC Gamer](https://pcgamer.com/starfield-doesnt-do-seamless-planetary-landings-like-no-mans-sky-but-todd-howard-doesnt-think-players-will-care)),
and players disagreed ([GamesRadar](https://www.gamesradar.com/starfield-players-are-already-tired-of-all-the-loading-screens-im-shocked-by-how-many-there-are)).
NMS streams the planet continuously around the player
([GDC 2017, McKendrick](https://www.gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-)).
Veach can't stop its tick (determinism, multiplayer), so it must follow NMS and bound the ship's speed by what
streaming can deliver. Everything in-system is seamless; only the interstellar jump may hide work.

## 3. Tradeoffs

| | Complexity | Performance | Determinism | Extensibility | Testability | Scale range |
|---|---|---|---|---|---|---|
| G2 SOI-primary gravity, fixed g_s | Low: one source plus volumes | One sqrt per entity per step | IEEE-exact ops only | Registry of source kinds | Closed form, easy to assert | Invariant by construction |
| G1 summed Newton | Low code, wrong frames | N sources per entity | Fine | Fine | Fine | g wrong by 1/s² or s |
| G3 authored curves | Low | Cheap | Fine | Curves as data | Fine | Must be re-tuned per s |
| F1 pure orbital | Medium, plus planning UI and time warp | Cheap | Fine | Module | Fine | Only small s without time warp |
| F2 assisted Newtonian | Medium: controller tuning | Cheap | Fine | Assist profiles as data | Replay of inputs | Works at every s |
| F3 arcade | Low | Cheapest | Fine | Little: mass and thrust stop mattering | Fine | Works at every s |
| C1 fixed speeds | Low | Streaming load fixed | Fine | Fine | Fine | Travel time ∝ s: fails |
| C3 v ≤ d/τ | Low: one nearest-body query | Streaming load constant in s | `ln` and `exp` need engine math (Q12) | τ, cap and significance threshold as data | Trip time τ·ln(ratio) is a closed-form oracle | Invariant |
| E1 physical heating | Low | Cheap | Fine | Fine | Fine | Nothing happens at small s |
| E2 relative heating | Low | Cheap | Fine | Curve as data | Fine | Same feel at every s |

## 4. Extension points

| What | Luau | C++ module | Toolbox core |
|---|---|---|---|
| Flight tuning: assist profiles, thruster curves, speed caps per ship class | Data and scripts | — | — |
| A whole flight model (orbital KSP-style, arcade, winged aero) | A single player ship's model in Luau is affordable (one per step) | Yes, through the `FlightModel` seam; needed for NPC fleets | The seam itself |
| Gravity source kinds (point, shape-aligned, box, cylinder for spin habitats, ring worlds, toroids) | Parameters; a Luau source evaluated per step only for a few entities | New kinds register with the gravity registry | The combination rule (SOI primary + volumes) |
| Gameplay gravity mapping per body kind (multiplier, floor, clamp) | A generator hook in Q5's pipeline, evaluated once per body | — | — |
| Travel drives and tiers (gates, highways, accelerators, jump variants) | Rules as data; transition sequences | New drive kinds | The cap law v ≤ max(v_floor, d/τ) and its enforcement (the "travel governor") |
| Entry heating, drag model, entry VFX | Curves and VFX parameters | Lift and aero models | Drag reads the shared atmosphere record |
| Docking and landing behaviours | Scripts | Autopilots | Frame handoffs (with Q1) |

The governor sits in the core on purpose: a modded flight model or drive can do anything it likes inside the cap, but
it cannot outrun streaming or break multiplayer fairness.

## 5. Interactions with the other questions

- **Q1 frames.** Surface physics in the body-fixed frame inside a rotation radius (the atmosphere top or a multiple
  of R), the inertial body-centred frame above it, the system frame in cruise. Velocity conversion v_in = v_body +
  ω × r is exact, so no visible jump, but the assist's reference changes there; do it at a fixed altitude so it is
  testable. In cruise the ship moves d/(60τ) per step (0.33% of d at τ = 5 s): camera-relative positions must be
  recomputed every frame, and absolute precision only needs to be relative to d.
- **Q2 mechanics and time.** SOI primaries come from Q2's orbit tree. The day-length validation (≥ some multiple of
  the surface orbital period) belongs in Q2's creator settings. Recommend **no time warp in multiplayer**: travel
  tiers give speed instead. Physics/rails handoff: a ship goes on rails only when no player controls it, it isn't
  thrusting, has no contacts and is outside any atmosphere; it comes back to physics at the current tick from its
  orbital elements when an interest region reaches it. Landed ships park in the body-fixed frame.
- **Q3 pipeline.** Supplies physical g at s = 1 (which the gameplay mapping starts from), the atmosphere record
  (ρ₀, H, top) and coarse shapes for irregular bodies.
- **Q4 terrain.** Receives the budget in section 7. Needs a "chunk ready" event and a collider radius around every
  physics body, plus the coarse SDF for shape-aligned gravity. The collider chunks near the ship are the hard
  deadline; visual LOD may lag.
- **Q5 procgen graph.** Gameplay-gravity mapping and gravity source kinds are registered like any other stage hook.
- **Q6 networking.** Flight is input-driven and deterministic, so client prediction with rollback re-simulates
  inputs through the same flight model. Under v = d/τ the dead-reckoning error for latency L is L/τ of the distance
  to the nearest body (2% at 100 ms, τ = 5 s) at every s. The server enforces the governor; jumps are server-side
  transitions.
- **Q7 simulation LOD.** Off-screen NPC traffic gets closed-form travel times: 2τ·ln(D/2h) per hop.
- **Q8 renderer.** Under the cap law, a target's apparent size grows by e every τ seconds at every s, so the
  impostor-to-mesh handoff, the atmosphere-shell switch and auto-exposure transitions happen at fixed angular sizes
  and fixed times regardless of s. Entry VFX read the heating value. **Conflict:** if Q8 scales the atmosphere top
  linearly with s (1 km at s = 0.01), natural orbital entry speed exceeds the budget there (v/h = 0.79 s⁻¹, needing
  τ ≥ 5 s → 0.2 s⁻¹). A √s rule (10 km at s = 0.01) keeps v_orb/h_atm = 0.079 s⁻¹ at every s. Recommend √s as the
  default; Q8 and Q2 own the final call.
- **Q9 physics.** Global gravity off; the toolbox applies per-body gravity as a force each step. Character
  controller with arbitrary up. No fictitious forces in body frames by default. Ship interiors in the ship's frame
  with "inertial dampening" on by default (thrust is not felt inside).
- **Q11 persistence.** A ship's saved state is frame id + local position (Q1 types) + velocity + tick + travel tier
  and drive state, so a save taken mid-cruise restores.
- **Q12 determinism.** sqrt is exact under IEEE; the cap law uses ln and the drag uses exp, so both need the engine's
  deterministic math, not libm.

## 6. M1-class vs M3 Pro-class

- **CPU cost of this question is negligible on both.** Gravity is one source plus a few volumes per entity per step;
  a flight model is a few hundred floating-point operations per ship per step; the nearest-body query runs over a
  handful of bodies per system. A thousand physics ships is not a CPU concern on either chip (*estimate, not
  measured*).
- **What differs is streaming throughput** (Q4's meshing on the GPU, readback to CPU colliders). M1 has 7–8 GPU
  cores and 4 performance CPU cores; M3 Pro has 14–18 GPU cores and 5–6 performance cores *(Apple specifications,
  not re-checked this session)*.
- **The rule that follows:** τ and v_floor are **gameplay constants, not hardware settings.** Travel speed must not
  depend on the GPU: it would make multiplayer unfair and replays depend on hardware. Set τ and v_floor from the
  M1-class budget; spend the M3 Pro's headroom on finer LOD (a smaller c₀ factor, more chunks per ring) at the same
  speed. When streaming falls behind, the renderer shows coarser LOD; it never slows the ship. Only collider chunks
  near the ship are a hard deadline.
- We can only measure on the M5 Max. Spike S10-1 measures throughput there with the meshing budget throttled
  per frame as a stand-in; the owner sets the real target from that.

## 7. Recommendation

1. **Gravity:** the SOI primary's inverse-square field, applied in that body's frame, with surface gravity chosen
   once per body: g_game = g_physical(at s = 1) × creator multiplier (default 1), clamped to a creator range.
   GM_game = g_game·R², so orbits, SOI and escape speed follow consistently. Linear inside the body. Shape-aligned
   direction near small bodies, with a creator floor. Artificial gravity as volumes that override natural gravity
   by priority, blended over ~0.5 m at the edge. All sources go through one registry.
2. **Character:** up = −ĝ with minimal-rotation blending (~0.15 s), no alignment below 0.05 m/s², parallel-transported
   heading, camera smoothed separately, body-fixed frame on surfaces, no fictitious forces by default, and a
   validation error when day length is too close to the surface orbital period.
3. **Flight:** a `FlightModel` seam (inputs: control intent from analog actions, ship mass and thrusters, gravity,
   air density, wind, frame; output: force and torque in the local frame, every fixed step). Default model: assisted
   Newtonian, assist-off available. Orbital and arcade models are modules. Analog input axes (mouse delta, gamepad)
   are an engine prerequisite.
4. **Travel:** three tiers in the core: local flight, in-system cruise, interstellar jump. One cap law across all of
   them, enforced by the travel governor: **|v| ≤ max(v_floor, d/τ)**, with v measured relative to the dominant
   body's surface frame and d the distance to the nearest significant surface. Cruise is a kinematic drive that can
   reach the cap far from bodies; it is disabled inside atmospheres, drops out on obstruction (Star Citizen's rule)
   and ignores bodies smaller than a minimum angular radius (so a rock doesn't trap a ship). The jump is the only
   place generation may be hidden, behind a transition of fixed length. No time warp in multiplayer.
5. **Entry:** drag from the shared atmosphere record; heating and VFX relative to local orbital speed (E2), mild by
   default, not destructive unless a creator turns it up.

### The approach-speed budget (what Q4, Q1 and Q8 size against)

Defaults: **τ = 5 s near bodies** (descent and atmospheric flight), **τ_c = 7 s in cruise**, **v_floor = 100 m/s**
below a few hundred meters. These are gameplay constants. The chunk counts use assumed LOD constants (finest chunk
under the ship c₀ = max(32 m, h/4), rings 8 chunks wide, ~64 chunks per level) that Q4 must replace with its own:

- **High band (h above ~128 m):** about **156/τ ≈ 31 chunks/s** sustained (78 at τ = 2 s, 16 at τ = 10 s),
  **independent of altitude and of s**. This is the central result: a distance-proportional cap makes the streaming
  load scale-invariant.
- **Low band:** ≈ 16·v/c_min chunks/s: **50 chunks/s at 100 m/s**, 150 at 300 m/s. A budget of B chunks/s allows
  v_floor = B·c_min/16: 60 m/s at B = 30, 120 m/s at B = 60, 300 m/s at B = 150.

| Quantity *(computed)* | s = 0.01 | s = 0.1 | s = 1 |
|---|---|---|---|
| Radius R | 63.7 km | 637 km | 6,371 km |
| Low-orbit speed √(gR), g = 9.81 | 791 m/s | 2.50 km/s | 7.91 km/s |
| Escape speed | 1.12 km/s | 3.54 km/s | 11.2 km/s |
| Surface orbital period (minimum safe day) | 8.4 min | 26.7 min | 84.4 min |
| Atmosphere top, √s rule (linear rule) | 10 km (1 km) | 31.6 km (10 km) | 100 km (100 km) |
| Max speed at the atmosphere top, d/τ | 2.0 km/s (0.2 km/s) | 6.3 km/s (2.0 km/s) | 20 km/s (20 km/s) |
| Max speed at h = 10 km / 1 km / 100 m | 2,000 / 200 / 100 m/s | same | same |
| Descent from h = R to 100 m at τ = 5 s | 32 s | 44 s | 55 s |
| Moon-like hop (384,400 km·s), h = R to h = R, τ_c = 7 s | 48 s | 48 s | 48 s |
| 1 AU·s hop, h = R to h = R (add the two descents for surface to surface: 3.3–4 min) | 131 s | 131 s | 131 s |
| Peak cruise speed on that hop | 0.36c | 3.6c | 36c |
| Peak entry deceleration (2°, H ∝ √s) | 0.48 g | 1.5 g | 4.8 g |
| Equatorial surface speed, 24 h day | 4.6 m/s | 46 m/s | 463 m/s |

The cross-checks are reassuring: natural orbital entry at s = 1 (7.9 km/s at 100 km, τ ≈ 13 s) is inside the
budget; the Space Shuttle's entry kept v/h around 0.03–0.07 s⁻¹ *(unverified figures)*; Elite's glide (~2.5 km/s at
~25 km, τ ≈ 10 s) is inside it; NMS's ~70–190 u/s atmospheric speeds sit near our v_floor.

**What would change my mind.** Q4 measuring cheap enough collider streaming that a 300 m/s floor is free (raise
v_floor). Playtests showing the exponential approach feels slow at the end, which is Elite's best-known complaint
(shorten τ in the last band). The owner wanting KSP realism as the default (make F1 the default, time warp in
single player only, and accept the governor interfering with low periapses at s = 1). The owner wanting NMS-style
arcade flight (make F3 the default; the seam doesn't change).

## 8. What is expensive to undo

- **The cap law and τ.** Once planet sizes, missions, NPC traffic timing, multiplayer and Q4's chunk budget assume
  v ≤ d/τ, changing it changes travel times and streaming everywhere at once.
- **The gravity combination rule** (SOI primary plus overriding volumes). Every physics test, recording and save
  depends on it.
- **Fixed gameplay g across s.** Creator content (jump heights, ship thrust, building loads) gets tuned against it.
- **The `FlightModel` seam's inputs and outputs.** Modules depend on it; adding fields is cheap, changing the frame
  or the step is not.
- **Body-fixed surface physics** (shared with Q1/Q9).
- Cheap to change: assist tuning, heating curves, v_floor, the orientation time constant, heading UI.

## 9. Proposed ADR titles

1. Gameplay gravity: the SOI primary's field per body, with surface gravity fixed across scale
2. Gravity sources are a registry; artificial gravity volumes override natural gravity
3. Characters align to local gravity; heading is parallel-transported, never derived from north
4. Flight models are modules behind one seam; assisted Newtonian is the default
5. The travel governor: speed capped at max(v_floor, d/τ), a gameplay constant streaming is sized against
6. Travel tiers: local flight, in-system cruise, interstellar jump; no time warp in multiplayer
7. Atmospheric drag from the shared atmosphere record; entry heating relative to local orbital speed
8. Analog input axes for flight (engine input extension; mouse delta and gamepads)

## 10. Spike candidates

Each is a throwaway prototype, sized by scope, with a visible success criterion. Order follows the first slice.

- **S10-1 Descent under the governor (no meshing).** A sphere at s = 0.01 and s = 1, a quadtree LOD selector on the
  CPU that only counts which chunks would be requested, and a Luau script that descends from h = R to 10 m under
  v ≤ max(v_floor, h/τ), recorded with `--record`. Scope: one script, one CPU LOD selector, Tracy plots. Success: the
  Tracy plot of chunk requests per second has the same peak within 20% at both scales, matches 156/τ for the
  assumed constants, and descent time matches τ·ln(R/h) within one second.
- **S10-2 Walk over a pole.** A capsule on a sphere collider with gravity toward the centre, walking a great circle
  over the pole and back, then stepping into an artificial-gravity box. Scope: depends on Q9's engine choice; one
  character controller, one gravity volume. Success: no heading flip at the pole (screenshot sequence), up-vector
  error under 1°, the gravity-volume transition finishes in ≤ 0.3 s with no camera snap.
- **S10-3 Hover and land on a rotating body.** An assisted-Newtonian ship in a body-fixed frame on a rotating sphere,
  with assist on/off swapped at run time by module and tuning changed over MCP `set_component`. Scope: one flight
  model, the seam, analog input via MCP injection. Success: hover drift under 0.1 m over 600 steps; `--replay`
  reproduces the component hashes exactly.
- **S10-4 Cruise between two bodies at two scales.** A kinematic cruise drive between two spheres D = 384,400 km·s
  apart. Scope: the governor, the nearest-body query, frame switches logged. Success: trip times within one second
  of each other at s = 0.01 and s = 1 and of 2τ_c·ln(D/2R); the log shows the same number of frame switches at both.
- **S10-5 (with Q4) Streaming throughput on the M5 Max.** Mesh real chunks while replaying S10-1's recording, with
  the meshing budget throttled per frame as an M1-class stand-in. Success: a Tracy plot of chunk backlog that never
  exceeds the collider radius's chunk count during the last 500 m; this sets τ and v_floor for the ADR.

## 11. Product decisions surfaced (realism vs gameplay)

| Decision | Recommendation |
|---|---|
| Surface gravity at compressed scale | Fixed at the physical s = 1 value (KSP precedent), creator multiplier, default 1 |
| Default flight model | Assisted Newtonian (Elite-like); orbital (KSP) and arcade (NMS) as optional modules |
| Travel time vs scale | Distance-proportional cruise, so trips take the same time at every s (~2–4 min per planet-to-planet hop) |
| Time warp | None in multiplayer; optional on-rails warp in single player is Q2's call |
| Entry heating | Gameplay-relative and mild by default; physical heating would vanish at small s |
| Loading between systems | Only the interstellar jump may hide work, behind a fixed-length transition; everything in-system is seamless |
| Rotating planets | Ignore fictitious forces; refuse day lengths near the surface orbital period |
| Ship interiors | Inertial dampening on by default (thrust not felt inside) |
| Atmosphere thickness vs s (with Q8/Q2) | Follow √s, not s, so orbital entry fits the streaming budget at small s |

## 12. Sources

Search-summary sources are marked in the notes; primary pages that refused fetches are listed with that caveat there.

- Outer Wilds gravity: [PC Gamer](https://www.pcgamer.com/how-to-build-a-black-hole/); [The Physics Mill](https://www.thephysicsmill.com/2024/09/20/an-astrophysicist-attempts-to-measure-the-physics-of-outer-wilds/)
- Space Engineers: [wiki: Planet](https://spaceengineerswiki.com/Planet); [Keen support thread](https://support.keenswh.com/spaceengineers/pc/topic/verifying-keens-law-of-gravity); [Mod API gravity component](https://keensoftwarehouse.github.io/SpaceEngineersModAPI/api/VRage.Game.ModAPI.IMySphericalNaturalGravityComponent.html); [Mod API provider system](https://keensoftwarehouse.github.io/SpaceEngineersModAPI/api/VRage.Game.ModAPI.IMyGravityProviderSystem.html); [Rosa/Petrzilka 2015](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/); [Rosa 2015 planets](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/)
- Elite Dangerous: [Supercruise](https://elite-dangerous.fandom.com/wiki/Supercruise); [Orbital Cruise](https://elite-dangerous.fandom.com/wiki/Orbital_Cruise); [Frontier forums on gravity wells](https://forums.frontier.co.uk/threads/shouldnt-gravity-wells-you-know-like-actually-work.346577/page-2); [Frontier quick start guide](https://community.elitedangerous.com/node/348); [Steam discussion on glide](https://steamcommunity.com/app/359320/discussions/0/595146546178222149); [Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe)
- No Man's Sky: [GDC 2017 McKendrick](https://www.gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-); [Jianshu write-up](https://www.jianshu.com/p/ea0fbd7abd6d); [NMS wiki: Pulsing Heart](https://nomanssky.gamepedia.com/Pulsing_Heart); [Steam discussion](https://steamcommunity.com/app/275850/discussions/0/412448792372600069)
- Star Citizen: [Quantum drive](https://starcitizen.tools/Quantum_travel); [Quantum travel mechanics](https://starcitizen.tools/Guide:Quantum_travel_mechanics); [PC Gamer 2014](https://www.pcgamer.com/au/big-changes-are-coming-in-the-star-citizen-alpha-20-release)
- KSP: [Physics warp](https://wiki.kerbalspaceprogram.com/wiki/Physics_warp); [Reference frames template](https://wiki.kerbalspaceprogram.com/wiki/Template:Reference_frames)
- X4: [Piloting and Travel](https://wiki.egosoft.com/X4%20Foundations%20Wiki/Manual%20and%20Guides/X4%3A%20Foundations%20Manual/Piloting%20And%20Travel/); [JPMFlight](https://wiki.egosoft.com/X4%20Foundations%20Wiki/Modding%20Support/Assets%20Modding/Guides/JPMFlight/)
- Starfield and Outlaws: [PC Gamer](https://pcgamer.com/starfield-doesnt-do-seamless-planetary-landings-like-no-mans-sky-but-todd-howard-doesnt-think-players-will-care); [TheGamer](https://www.thegamer.com/starfield-wont-feature-seamless-space-planet-landing/); [GamesRadar on loading screens](https://www.gamesradar.com/starfield-players-are-already-tired-of-all-the-loading-screens-im-shocked-by-how-many-there-are); [GamesRadar on the Seamless Landing mod](https://www.gamesradar.com/games/rpg/seamless-landing-mod-looks-set-to-bring-starfield-another-step-closer-to-no-mans-sky/); [GamesRadar on Outlaws](https://www.gamesradar.com/games/action/as-star-wars-outlaws-no-loading-screens-come-under-scrutiny-its-director-already-reminded-us-that-having-seamless-planetary-flying-wouldve-been-lots-of-work-for-little-payoff)
- Jolt: [Jolt JNI character docs](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/character.html); [Jolt samples](https://jrouwe.github.io/JoltPhysics/md__docs_2_samples.html)
- Veach: [ADR 0013](../adr/0013-conventions-and-math.md), [ADR 0018](../adr/0018-engine-lifecycle-and-phases.md), [input spec](../specs/input/spec.md), [00-scope](00-scope.md)
