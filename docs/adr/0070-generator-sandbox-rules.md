# 0070: Rules for Luau generators, from SP-07

Date: 2026-11-27

## Status

Accepted. Amends the Luau procgen sandbox of [0052](0052-procgen-runtime.md) and the Luau rules of
[0043](0043-determinism-tiers-and-strict-floating-point.md). Validated by SP-07 (VCH-083,
[findings](../research/spikes/sp-07-luau-generator.md)).

## Context

SP-07 built the generator sandbox (`engine/script`, until `engine/procgen` exists): a separate VM per sandbox, no world
access, `math` rebound to `veach::dmath` with the compiler's `disabledBuiltins` closing the fastcall path to the
platform's libm, randomness only from the context's counter-based hash, an interrupt budget, interpreted only. A
climate and biome hook over a 64² × 6 cube map gives the same bits in Debug and Release and as a C++ port of it, at
2.06 µs per cell. It also found what the sandbox alone cannot close.

## Decision

1. **`^` is rejected in generators** at load time, with a hint to use `math.pow`. Luau lowers `^` to the platform's
   `pow` and its compiler folds constant `^` with the host's libm; patching vendored Luau would not fix the folding.
   Gameplay scripts keep `^`.
2. **Budgets:** an instruction budget counted at deterministic safepoints (loop back edges, calls, returns) and a
   memory budget, both failing with `procgen.budget_exceeded`, even inside `pcall`. A wall-clock budget exists only as
   a hang guard: off by default, and when it fires the stage fails; it never changes an output. Safepoint counts are
   versioned with Luau.
3. **A fresh closure per invocation**, so a hook's results never depend on earlier calls.
4. **Rules a hook must follow** (enforced where cheap, documented in the generator API):
   - output must not depend on `pairs` order over tables keyed by tables, functions or userdata (their order follows
     memory addresses); string and number keys are deterministic;
   - buffer writes take integers in range; a fractional or out-of-range value converts differently per platform, and
     debug builds check it;
   - decimal literals are parsed by the C library's `strtod` (Apple libc and the UCRT are correctly rounded); the
     committed hashes check it on every platform, and a failure there moves literal parsing into the engine.
5. **Native code generation stays off for generators** until a spike shows arm64 and x64 lowerings agree bit for bit.

## Consequences

- Creators get a strict, typed Luau surface whose outputs are canonical across Mac and Windows; mistakes that would
  break determinism fail at load or in debug builds, not silently.
- Gameplay scripts are unaffected (replay tier).
- A generator type environment for `luau-analyze` (no `math.random`, `math.noise`, `os` or `^`) is the next step, so
  these mistakes show in the editor.

## Alternatives considered

- **Route `^` to dmath** by patching `luai_numpow`: still folded by the compiler with host libm, and a patch to carry.
- **A wall-clock budget as the limit:** non-deterministic; a hook would succeed on one machine and fail on another.
- **Seeded string hashing to randomize `pairs` order in tests:** catches some bugs but costs a Luau patch; the rule plus
  canonical-hash tests is enough today.
