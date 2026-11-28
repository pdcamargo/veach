# 0081: Creator control over atmosphere scaling and sunsets; s_a = √s_p stays the default

Date: 2026-11-27

## Status

Accepted (the owner's decision after SP-10). Amends [0049](0049-scale-model.md) and [0063](0063-astronomical-rendering.md).

## Context

SP-10 ([findings](../research/spikes/sp-10-atmosphere.md)) showed that s_a = √s_p keeps the overhead sky identical at
every scale but pales sunsets at small scale: the horizontal path through the air goes as s_p^¼ (sun 1.5° up, blue
transmittance 0.006 at s = 1, 0.029 at s = 0.1, 0.103 at s = 0.01). The owner reviewed the s = 1, 0.1 and 0.01 renders.
s_a = √s_p also matches relief exponent 0.5 (confirmed by the owner, VCH-085): summits sit at the same depth in the air
as on Earth (35 % of surface pressure on the tallest peak at every s), where s_a = s_p would leave 4 % at s = 0.1.
The owner kept the rule and asked for it to be configurable, hookable and extensible by creators.

## Decision

1. **Default unchanged:** s_a = √s_p, matched to the default relief exponent 0.5.
2. **Universe setting: the atmosphere exponent k**, s_a = s_p^k, in [0.5, 1], stored in the genesis next to the relief
   exponent and immutable once anchored, like every scale factor. The editor shows the summit pressure the pair gives
   and warns when k differs from the relief exponent.
3. **Per-body control through a realization hook**: a creator Luau hook (the generator sandbox's rules,
   [0070](0070-generator-sandbox-rules.md)) may multiply a body's realized atmosphere thickness and scale heights. It
   runs where scale is applied, when records become geometry, so generation stages still never read s
   ([0049](0049-scale-model.md)); its output is canonical and hashed like any hook's.
4. **A stylized sunset term**, off by default: extra extinction growing with air mass, applied in the transmittance
   table, set per universe with a per-body override in the same hook. It is visual-tier only
   ([0043](0043-determinism-tiers-and-strict-floating-point.md)) and never feeds gameplay (drag, pressure, heating).

## Consequences

- Physical defaults stay; creators who want deep sunsets on small planets get them without changing flight, clouds or
  pressure.
- The atmosphere record gains a visual-parameters block, separate from its physical values.
- Backlog: VCH-109, built with the real atmosphere after the first slice.

## Alternatives considered

- **A fixed rule with no control:** simpler, but the owner wants creators to tune the look.
- **Arbitrary s_a per body from generation:** breaks "no stage reads s" and the CI check that stage hashes are equal
  across scales.
