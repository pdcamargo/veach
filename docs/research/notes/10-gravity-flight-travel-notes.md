# Q10 raw notes: gravity, flight and travel

Researcher notes for [../10-gravity-flight-travel.md](../10-gravity-flight-travel.md). Collected 2026-11-27.

**How to read the sources.** Several primary pages refused automated fetches this session (the KSP wiki behind a
bot wall, the Elite fandom wiki with HTTP 402, PC Gamer's article body truncated, Frontier's old community guide
redirected). Where a finding below comes only from a search-engine summary of a page rather than the page itself, it
is marked *(search summary)*. Anything from memory, not re-checked this session, is marked *(unverified)*. Numbers
computed here are marked *(computed)* and the script is reproduced at the end.

## 1. Gravity model: per-body, sphere of influence, small bodies, artificial gravity, physical vs gameplay

### Takeaway
Every precedent that ships walkable planets uses gameplay gravity, not Newton: KSP keeps ~1 g on a planet a tenth of
Earth's radius, Outer Wilds uses linear falloff for planets and inverse-square only for the sun, Space Engineers
truncates its wells with a 7th-power falloff. Veach should compute physical gravity once (Q3) and then pick a gameplay
surface g per body that does not change with s.

### Cited Findings
- Outer Wilds applies the inverse-square law only to the sun; for planets the team used a "more linear decline"
  because inverse-square in a tiny system made escape velocity "too easy to achieve"; the developers also use
  "alignment fields" that orient the player toward masses, and describe "a sphere of influence for the sun and ...
  spheres of influence for each planet" — [PC Gamer, "Recreating gravity in games is harder than you'd think"](https://www.pcgamer.com/how-to-build-a-black-hole/) *(search summary; the fetched page body was truncated)*.
- Outer Wilds: floating-point error builds up if the player flies far out with autopilot off, so the star map breaks
  down — same PC Gamer article *(search summary)*.
- An astrophysicist's measurement of Outer Wilds' physics (planet gravity scales linearly) — [The Physics Mill, 2024](https://www.thephysicsmill.com/2024/09/20/an-astrophysicist-attempts-to-measure-the-physics-of-outer-wilds/) *(search summary only)*.
- Space Engineers natural gravity: g = g_base × (MaxR / r)^7 beyond the planet's maximum radius, constant g_base
  between min and max radius; MaxR is a per-planet definition value — [Space Engineers wiki: Planet](https://spaceengineerswiki.com/Planet) *(search summary)*. A Keen support thread works the same formula for EarthLike (60 km radius, +12% elevation → 67.2 km effective; ~0.0886 g at 95 km from the centre) — [Keen support: Verifying Keen's Law Of Gravity](https://support.keenswh.com/spaceengineers/pc/topic/verifying-keens-law-of-gravity) *(search summary)*. The wiki's other "71.82% of the radius" phrasing conflicts with the 1.12 factor; unresolved.
- Space Engineers' mod API exposes a spherical natural gravity component with min radius, max radius, falloff and
  intensity — [IMySphericalNaturalGravityComponent](https://keensoftwarehouse.github.io/SpaceEngineersModAPI/api/VRage.Game.ModAPI.IMySphericalNaturalGravityComponent.html) and a provider registry [IMyGravityProviderSystem](https://keensoftwarehouse.github.io/SpaceEngineersModAPI/api/VRage.Game.ModAPI.IMyGravityProviderSystem.html) *(search summary)*. This is a precedent for a gravity-source registry.
- Space Engineers 2015 plan: planet gravity "similar to earth gravity", decreasing with distance, bigger planets have
  more gravity, applied to ships too; planets 20–50 km then (example 100 km radius), max speed 104 m/s, ~12 minutes
  from view to surface — [Marek Rosa blog, Petrzilka guest post](https://blog.marekrosa.org/2015/04/guest-post-by-ondrej-petrzilka-space_17/) (fetched).
- KSP: Kerbin is ~600 km radius with ~1 g surface gravity *(unverified this session; widely documented; also in 00-scope)*.
- Space Engineers artificial gravity adds to natural gravity and is weakened inside it *(unverified)*.

### Inferences
- With surface g fixed and radius scaled by s, GM = g·R² ∝ s². Sphere of influence r_SOI = a·(m/M)^0.4 then scales
  ∝ s when both planet and star follow the same rule, so SOI/R stays ~145 (Earth's ~924,000 km SOI / 6,371 km)
  *(computed from the textbook formula)*.
- On-rails planets are in free fall around their star, so in a planet's frame the star's pull is cancelled except
  for tides (~10⁻⁶ g near Earth). Summing every body's gravity in a body frame is wrong; use the SOI primary only
  (patched conics) plus artificial volumes.
- Inside a body (caves to the core), uniform-density gravity falls linearly to zero at the centre: g(r) = g_s·r/R.
- Small irregular bodies: a 500 m asteroid at density 2,000 kg/m³ has g = (4/3)πGρR ≈ 2.8×10⁻⁴ m/s² *(computed)*:
  physically you can't walk. Needs a creator floor or "mag boots", and a direction that follows the shape near the
  surface (gradient of the coarse SDF) blending to the centre farther out.

### Gaps
- No primary source fetched for Space Engineers' rule for combining artificial and natural gravity.
- No source found for how NMS computes planet gravity (it appears constant per planet; unverified).

## 2. Player and character on a sphere

### Takeaway
Local up = −ĝ, blended over a short time constant, with no re-orientation in near-zero g. Walk in the body-fixed frame
so the ground is static. Heading must be parallel-transported, never derived from north, or it flips at the poles.

### Cited Findings
- Outer Wilds orients the player toward masses with authored "alignment fields" — [PC Gamer](https://www.pcgamer.com/how-to-build-a-black-hole/) *(search summary)*.
- Jolt has two characters: `Character` (a dynamic rigid body) and `CharacterVirtual` (no body, updated explicitly
  after each simulation step via `ExtendedUpdate`) — [Jolt JNI character docs](https://stephengold.github.io/jolt-jni-docs/jolt-jni-en/English/character.html) *(search summary)*. That `CharacterVirtual` takes an arbitrary up vector (`SetUp`) is *(unverified this session)*.
- Spherical gravity fields point toward their centre, uniform ones in a fixed direction; Unity's Catlike Coding
  "Custom Gravity" tutorial is the standard walkthrough of up-vector alignment *(unverified; URL https://catlikecoding.com/unity/tutorials/movement/custom-gravity/ not fetched)*.
- Space Engineers building grids can't stay axis-aligned on planets ([Rosa](https://blog.marekrosa.org/2015/11/planets-because-you-wanted-them_12/), via 00-scope).

### Inferences
- Rotating planets *(computed)*: equatorial surface speed at a 24 h day is 4.6 m/s (s = 0.01), 46 m/s (0.1),
  463 m/s (1). Centrifugal acceleration ω²R is 0.0003, 0.003, 0.034 m/s² (≤ 0.3% of g): ignoring fictitious forces
  in the body-fixed frame is invisible at physical day lengths.
- With a 30-minute day: centrifugal 0.78 m/s² (8% g) at s = 0.01, 7.8 m/s² (79% g) at s = 0.1 and 78 m/s² (more
  than g: the planet would fly apart) at s = 1; Coriolis at 300 m/s is 2.1 m/s² at any s. Day length must be
  validated against the surface orbital period 2π√(R/g) (8.4 min, 26.7 min, 84.4 min).
- Camera up should be smoothed separately from the capsule's up, so stepping between gravity volumes doesn't snap
  the view.

### Gaps
- Box3D's character mover and arbitrary-up support: not researched (Q9 owns the engine choice).

## 3. Flight models

### Takeaway
Assisted Newtonian (Elite, Star Citizen) is the only family that is physically consistent, works with gravity and
ship building, and stays playable at every s because it never asks the player to reach orbital speed. Pure orbital
(KSP) only works with time warp; arcade (NMS) throws away mass and thrust.

### Cited Findings
- Elite Dangerous normal flight ~300 m/s, with supercruise and hyperspace as separate tiers — [Space.com](https://www.space.com/entertainment/space-games/why-space-games-still-struggle-with-the-scale-of-the-universe) (via 00-scope).
- NMS in-atmosphere ship speed: average 70 u/s, accelerating to 150 u/s, braking to 20 u/s, boost 190 u/s; space
  boost up to 2,200 u/s; pulse ~40,000 u/s (older version 20,000) — [NMS wiki: Pulsing Heart](https://nomanssky.gamepedia.com/Pulsing_Heart) *(search summary; "u" is the game's unit, its mapping to meters is unverified)*. An older copy gives 35 u/s average and 50 u/s top in atmosphere *(search summary)*.
- NMS auto-stops pulse when nearing "any rendering object, like a planet" (forum) — [Steam discussion](https://steamcommunity.com/app/275850/discussions/0/412448792372600069) *(search summary, user claim)*. NMS re-entry heat is "only an animation" (forum, user claim, unverified).
- NMS planets stream continuously: the planet is generated locally around the player and kept updated like texture
  streaming — [GDC 2017, Innes McKendrick, "Continuous World Generation in No Man's Sky"](https://www.gamedeveloper.com/programming/video-how-continuous-world-generation-works-in-i-no-man-s-sky-i-); [Jianshu write-up](https://www.jianshu.com/p/ea0fbd7abd6d) *(search summary)*.
- X4 travel mode: engine choice sets charge time and top speed; travel drives charge ~3 s; parameters `charge` and
  `attack` (time to max travel speed); boosting during charge; shield loss disables travel mode; highways,
  superhighways, gates and accelerators — [Egosoft X4 manual: Piloting and Travel](https://wiki.egosoft.com/X4%20Foundations%20Wiki/Manual%20and%20Guides/X4%3A%20Foundations%20Manual/Piloting%20And%20Travel/); [JPMFlight modding guide](https://wiki.egosoft.com/X4%20Foundations%20Wiki/Modding%20Support/Assets%20Modding/Guides/JPMFlight/) *(search summary)*.
- Star Citizen IFCS (intelligent flight control) converts stick input to target rates within thruster limits *(unverified; no primary source fetched)*.

### Inferences *(computed)*
- With fixed g = 9.81 m/s²: R = 63.7 / 637 / 6,371 km; low-orbit speed √(gR) = 791 m/s / 2.50 km/s / 7.91 km/s;
  escape 1.12 / 3.54 / 11.2 km/s; surface orbit period 8.4 / 26.7 / 84.4 min, at s = 0.01 / 0.1 / 1.
- s = 0.1 gives 2.5 km/s, close to KSP's Kerbin low orbit (~2.2–2.3 km/s, unverified), which is a sanity check that
  the √s rule reproduces KSP.
- An assisted model that hovers against gravity and hands off to a non-Newtonian cruise drive makes orbital speed
  irrelevant to the default game, so √s stops mattering for the player.

### Gaps
- No primary source fetched on Star Citizen's IFCS or Elite's flight-assist tuning.

## 4. Travel tiers and speed caps near bodies

### Takeaway
Elite caps supercruise speed by proximity to massive bodies; players fly a "time-to-target" rule. A cap proportional
to distance (v ≤ d/τ) gives exponential approach, travel time logarithmic in distance, and a streaming load that is
independent of s. That is the founding property for Veach.

### Cited Findings
- Elite supercruise maximum 2001c needs great distance from massive bodies; maximum speed "is determined by the
  proximity to stellar bodies and the direction the ship is heading": heading toward a body lowers it, away raises it
  — [Elite wiki: Supercruise](https://elite-dangerous.fandom.com/wiki/Supercruise) *(search summary; page refused fetch with 402)*.
- Frontier's manual ("Variable Maximum Speed"): top speed varies with proximity and mass of nearby stellar bodies;
  a forum post (not a named developer) describes the local maximum as roughly the overall maximum divided by local
  field strength — [Frontier forums](https://forums.frontier.co.uk/threads/shouldnt-gravity-wells-you-know-like-actually-work.346577/page-2) *(search summary)*.
- Elite orbital cruise: lowest altitude ~25 km; dipping under the minimum drops to normal space — [Elite wiki: Orbital Cruise](https://elite-dangerous.fandom.com/wiki/Orbital_Cruise) *(search summary)*.
- Frontier's quick-start guide: the target planet shows the maximum speed for a safe transition into orbital
  cruise; too fast at the transition height forces an emergency drop; descending to the drop-out height starts an
  orbital glide — [Frontier quick start guide](https://community.elitedangerous.com/node/348) *(search summary; URL now redirects to Galnet)*.
- Glide: a player says ≤5 km/s is needed to enter glide and glide forces ~2.5 km/s — [Steam discussion](https://steamcommunity.com/app/359320/discussions/0/595146546178222149) *(search summary, user claim)*; another source cites a 200 km/s safe approach indicator (conflict; different phases, unresolved).
- Elite's community "seven-second rule" for supercruise approach (throttle so time-to-target reads 0:07) *(unverified; from memory)*.
- Star Citizen quantum drives cruise at 53.6 Mm/s (0.18c) to 283 Mm/s (0.94c); exact behaviour "not publicly
  known"; phases accelerate/cruise/decelerate; quantum travel can end at the upper atmosphere of planets and moons;
  an old article says the computer pulls you out if a collision is possible — [starcitizen.tools: Quantum drive](https://starcitizen.tools/Quantum_travel); [Guide: Quantum travel mechanics](https://starcitizen.tools/Guide:Quantum_travel_mechanics); [PC Gamer 2014](https://www.pcgamer.com/au/big-changes-are-coming-in-the-star-citizen-alpha-20-release) *(search summary)*.
- KSP limits time warp by altitude: over 4× not allowed below 70 km near Kerbin (the atmosphere top); physics warp
  is separate — [KSP wiki: Physics warp](https://wiki.kerbalspaceprogram.com/wiki/Physics_warp) *(search summary)*; per-body warp altitude tables in [Template:Reference frames](https://wiki.kerbalspaceprogram.com/wiki/Template:Reference_frames) *(not readable this session)*.

### Inferences *(computed)*
- Under v = d/τ the time to go from d₀ to d₁ is τ·ln(d₀/d₁). A body-to-body trip of separation D ending at distance
  h_e each side takes ≈ 2τ·ln(D/2h_e). With τ_c = 7 s and h_e = R: Earth–Moon-like (D = 384,400 km·s) 48 s;
  1 AU·s hop 131 s — the same at s = 0.01, 0.1 and 1 because every length scales together.
- Peak cruise speed midway on a 1 AU·s hop: 0.36c (s = 0.01), 3.6c (s = 0.1), 36c (s = 1). Below Elite's 2001c.
- Radial descent from h = R to 100 m at τ = 5 s: 32 s (s = 0.01), 44 s (0.1), 55 s (1).

### Gaps
- No official Elite formula; the forum's "max/field strength" is a paraphrase.
- NMS unit-to-meter mapping and its planet approach cap not found.

## 5. Atmospheric entry, drag and heating

### Takeaway
Physical entry heating and deceleration both collapse at small s (energy per kg ∝ v² ∝ s), so entry drama at
compressed scale has to be gameplay, defined relative to the body's own orbital speed.

### Cited Findings
- Sutton–Graves stagnation heating q ∝ √(ρ/r_n)·v³ and Allen–Eggers peak ballistic deceleration a = v²·sinγ/(2eH)
  are the textbook models *(unverified this session; standard aerospace references)*.
- Space Shuttle entry, roughly Mach 25 at 120 km, ~Mach 10 near 55 km, ~Mach 2.5 near 25 km *(from memory, unverified)*:
  v/h stays around 0.03–0.07 s⁻¹ through entry.

### Inferences *(computed)*
- Natural entry ratio v_orb/h_atm: with atmosphere top h_atm = 100 km·√s it is 0.079 s⁻¹ at every s (τ ≈ 12.7 s);
  with h_atm = 100 km·s it is 0.79 s⁻¹ at s = 0.01 (τ ≈ 1.3 s), too fast for the streaming law below.
- Allen–Eggers, γ = 2°, H = 8.5 km·√s: 0.48 g (s = 0.01), 1.5 g (0.1), 4.8 g (1). With H ∝ s it is 4.8 g at every s.
- Physical heat load per kg ∝ v² ∝ s: at s = 0.01 entry carries 1% of Earth's energy per kilogram.

### Gaps
- KSP's heating model was not readable this session.

## 6. Transitions with no loading screen; Starfield as counterexample

### Cited Findings
- Starfield does not let players fly seamlessly from orbit to surface; Todd Howard framed it as an RPG priority and
  "not that important to the player" — [PC Gamer](https://pcgamer.com/starfield-doesnt-do-seamless-planetary-landings-like-no-mans-sky-but-todd-howard-doesnt-think-players-will-care); [TheGamer](https://www.thegamer.com/starfield-wont-feature-seamless-space-planet-landing/) *(search summary)*. Players found the loading screens numerous — [GamesRadar](https://www.gamesradar.com/starfield-players-are-already-tired-of-all-the-loading-screens-im-shocked-by-how-many-there-are) *(search summary)*.
- A modder's account: behind Starfield's landing screen "the game tick is completely stopped while space is torn
  down and the area you're landing in is being built" — [GamesRadar on the Seamless Landing mod](https://www.gamesradar.com/games/rpg/seamless-landing-mod-looks-set-to-bring-starfield-another-step-closer-to-no-mans-sky/) / [FRVR](https://frvr.com/blog/news/i-couldnt-get-it-out-of-my-mind-starfield-seamless-landings-modder-explains-how-they-brought-the-mind-blowing-wishlist-feature-to-bethesdasci-fi-rpg/) *(search summary; modder claim)*.
- Star Wars Outlaws' director: seamless planetary flight would have been "lots of work for little payoff" — [GamesRadar](https://www.gamesradar.com/games/action/as-star-wars-outlaws-no-loading-screens-come-under-scrutiny-its-director-already-reminded-us-that-having-seamless-planetary-flying-wouldve-been-lots-of-work-for-little-payoff) *(search summary)*.

### Inferences
- Stopping the tick to build the destination is exactly what Veach's fixed-step determinism and multiplayer cannot do
  mid-system. The only acceptable hidden load is the interstellar jump, whose length is a gameplay constant.

## 7. Streaming budget model *(computed; all constants are assumptions Q4 must replace)*

- Model: finest LOD chunk under the ship c₀(h) = max(c_min, h/4), c_min = 32 m; each LOD level is a ring 8 chunks
  wide (~64 chunks). New chunks per second ≈ 16·v_h/c₀ + 64·|v_r|/(h·ln 2).
- Under v_h = v_r = h/τ (above ~128 m): ≈ 156/τ chunks/s, independent of h and of s: 78 (τ = 2 s), 31 (5 s),
  22 (7 s), 16 (10 s).
- Low band (h < 4·c_min): ≈ 16·v/c_min, so a budget B chunks/s allows a floor speed v_floor = B·c_min/16:
  60 m/s (B = 30), 120 m/s (B = 60), 300 m/s (B = 150).
- Apparent angular size of the target grows by a factor e every τ seconds during approach, at every s.

## 8. Determinism, networking, testability

- ADR 0018: simulation at fixed 1/60 s, `veach::Random`, record/replay of inputs; headless runs one step per frame.
- Input spec: actions are digital; `axis()` returns −1, 0 or 1; mouse motion and gamepads are out of scope
  (VCH-036). Flight needs analog axes: an engine input extension, not toolbox work.
- Networking (inference): relative dead-reckoning error under v = d/τ with latency L is L/τ of the distance to the
  nearest body (2% at 100 ms and τ = 5 s), independent of s.

## Script used for the computed numbers

```python
import math
g=9.81; R0=6.371e6; tau=5; tc=7
for s in (0.01,0.1,1):
    R=R0*s; vo=math.sqrt(g*R); hs=100e3*s**.5; hl=100e3*s
    print(s, R, vo, vo*2**.5, 2*math.pi*math.sqrt(R/g)/60,    # R, v_orb, v_esc, T_orb (min)
          hs, hs/tau, hl, hl/tau,                               # h_atm (sqrt, linear) and caps there
          tau*math.log(R/100),                                  # descent R -> 100 m
          2*tc*math.log(1.496e11*s/(2*R)), 2*tc*math.log(3.844e8*s/(2*R)))  # AU and Moon hops
```
