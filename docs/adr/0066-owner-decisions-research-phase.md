# 0066: The owner's product decisions after the research phase

Date: 2026-11-27

## Status

Accepted. A product decision by the owner (2026-11-27), answering the research phase's review packet
([research/00-review-packet.md](../research/00-review-packet.md)). Supersedes the performance goal of
[0003](0003-platforms-and-hardware-target.md) and confirms the working defaults of [0049](0049-scale-model.md),
[0056](0056-server-authoritative-multiplayer.md), [0059](0059-gravity-flight-travel.md) and
[0064](0064-quality-presets.md).

## Context

The research phase ([synthesis](../research/00-synthesis.md)) fixed the toolbox's engineering in ADRs 0043 to 0065
and left six product questions to the owner, each with a recommendation. The owner accepted all six.

## Decision

1. **Hardware target: 60 fps at 1440p with every feature on, on an M3 Pro-class GPU.** This is the design target
   every renderer budget is set against ([0060](0060-gpu-driven-visibility-buffer.md) to
   [0064](0064-quality-presets.md)). A base M1-class GPU at 1080p60 on the low preset stays the working minimum, estimated
   only: the owner will not buy an M1 for testing. The M5 Max measures, and the research's scaling factors (M3 Pro
   about 2.5× slower, M1 about 6×) estimate; VCH-097 measures an M3 Pro-class machine if one becomes available.
2. **Scale:** default `s = 0.1`, supported range 0.01 to 1, one setting for planets and orbits (`s_o = s_p`) with orbits
   separable as an advanced setting; scale locks once anything in a universe is anchored ([0049](0049-scale-model.md),
   [0055](0055-universe-identity-and-versioning.md)).
3. **Creator defaults:** surface gravity ×1; day length ×1 within the rotation limits of 0049; year length follows
   gravity (√s); atmosphere thickness `s_a = √s_p`; relief exponent 0.5 (confirmed by eye in VCH-085); habitable
   planets common (a generous habitability bias); dig brushes capped at 16 m; cave band 500 m; human-scale haze on.
4. **Multiplayer:** co-op, Mac and Windows together; up to 4 players on a listen server and 8 to 16 on a dedicated one,
   with no limit in any format; dedicated server first; identical mods required ([0056](0056-server-authoritative-multiplayer.md)).
5. **Realism vs gameplay:** real surface gravity; orbital speeds and periods follow √s; equal travel times at every
   scale (the governor of [0059](0059-gravity-flight-travel.md)); no time warp in multiplayer, up to 100,000× in single
   player ([0047](0047-universe-clock.md)); no Coriolis or centrifugal effects on foot; mild entry heating; jumps
   between star systems, no continuous interstellar flight; a 100,000 light-year galaxy by default.
6. **First slice (VCH-091):** one seeded system (star, planet, moon), the planet rotating and tilted, its atmosphere
   from its composition, the right local time, flying down with no loading screen, landing and digging, at `s = 0.01`
   and `s = 1`, single player. Out: multiplayer, fauna and flora, oceans, clouds, vegetation, ship interiors,
   background simulation, any galaxy beyond a starfield.

## Consequences

- Renderer budgets are set for M3 Pro-class at 1440p; the M1 low preset is a scaled-down configuration, not the design
  point. ARCHITECTURE.md's non-negotiable and VISION.md's rendering goals say so.
- The spikes (VCH-077 to VCH-091) proceed in order toward the first slice.

## Alternatives considered

The packet's alternatives for each point; the owner chose the recommendation every time.
