## Review packet: product decisions from the space toolbox research phase

**Answered by the owner on 2026-11-27: every recommendation accepted, with M3 Pro-class at 1440p60 as the target.
Recorded in [ADR 0066](../adr/0066-owner-decisions-research-phase.md).**

- **Decision needed:** set the hardware target, the universe scale defaults, the multiplayer scope, a few realism vs
  gameplay rules, and what goes into the first playable slice.

- **Context:** Veach is a C++ game engine becoming the base of a toolbox for No Man's Sky-style space games: seeded
  galaxies, real orbits, planets you can fly down to and dig into, multiplayer. The research phase compared shipped
  games (No Man's Sky, Elite Dangerous, Star Citizen, Kerbal Space Program, Space Engineers, Outer Wilds) and papers,
  and fixed the engineering in 23 decision records. "Scale" is a creator setting `s` that shrinks planets and orbits: `s = 1` is real size, `s = 0.1`
  gives an Earth-like planet a 637 km radius (Kerbal Space Program's scale). Generation runs at real size, so a seed
  gives the same planets at any scale.

- **Options and recommendations:**
  1. **Hardware target.** The current target (60 fps at 1080p on a base M1, everything on) does not fit: the full
     feature set (clouds, ocean, vegetation) is estimated at about 19 ms per frame on a base M1, against a 16.7 ms
     budget. I recommend designing for an M3 Pro-class Mac at 1440p60 with everything on, with a base M1 at 1080p60 on
     a low preset as the minimum. Also buy a used base M1 and an M3 Pro-class Mac, so these numbers become measurements instead of estimates scaled from the M5 Max.
  2. **Default scale and range.** Default `s = 0.1`, supported range 0.01 to 1, one setting for planets and orbits
     together (orbits can be split off as an advanced setting). Scale locks once anything in a universe has been built
     or edited; changing it afterwards means a new universe.
  3. **Creator settings and their defaults.** Surface gravity real (×1). Day length real (×1), refused when a planet
     would spin fast enough to throw players off. Year length follows from gravity (115 days at `s = 0.1`). Atmosphere
     thickness scales with √s (10 km at `s = 0.01`), which keeps the sky's color. Mountains shrink less than the
     planet (880 m instead of 88 m at `s = 0.01`, to be confirmed by eye in a prototype). Habitable planets are common,
     as in No Man's Sky. Dig brushes are capped at 16 m and caves reach 500 m deep.
  4. **Multiplayer scope.** Co-op, with Mac and Windows players together. Up to 4 players when a player hosts, 8 to 16
     on a dedicated server, and no player limit in any file or network format. Dedicated server first, because it can
     be tested automatically. Everyone must run the same mods.
  5. **Realism vs gameplay.** Real surface gravity. Orbits get faster at small scale (they follow √s) rather than
     staying real. Travel takes the same time at every scale. No time warp in multiplayer (up to 100,000× in single
     player). No Coriolis or centrifugal effects on foot. Mild entry heating. Jumps between star systems instead of
     continuous interstellar flight. A Milky Way-sized galaxy (100,000 light years) by default.
  6. **First slice.** In: one seeded system (star, planet, moon), the planet rotating and tilted, its atmosphere coming
     from its composition, the right local time where you stand, flying down with no loading screen, landing and
     digging, at both `s = 0.01` and `s = 1`, single player. Out: multiplayer, creatures and plants, oceans, clouds,
     vegetation, ship interiors, background simulation, and any galaxy beyond a starfield.

- **Risks and reversibility:** scale and the generation rules are hard to change once players have saves, because a
  world pins its generator versions when it is first edited. The hardware target is cheap to change now and expensive
  once the renderer is built around it. Player counts are not baked into any format.

- **Evidence:** the research is in `docs/research/` (scope analysis, 12 questions, a synthesis), the decisions in
  `docs/adr/0043` to `0065`, and the 21 prototypes to run next in the backlog (VCH-077 to VCH-097). The M1 and M3 Pro
  frame times are estimates scaled from the M5 Max (M1 about 6× slower, M3 Pro about 2.5×): full features take 18.9 ms
  on an M1 at 1080p, 8 ms on an M3 Pro at 1080p and 11.9 ms on an M3 Pro at 1440p.

- **Questions for the owner:**
  1. Hardware: M3 Pro 1440p60 as the design target, base M1 1080p60 low as the minimum (yes/no)? Buy the test Macs?
  2. Default scale 0.1, range 0.01 to 1, locked once edited (yes/no, or give a number)?
  3. The defaults in point 3 (yes/no; name any you want changed)?
  4. Multiplayer: co-op, 4 hosted and 8 to 16 dedicated, dedicated first (yes/no, or set the numbers)?
  5. The realism rules in point 5 (yes/no per rule)?
  6. The first slice as listed (yes/no; move items in or out)?
