# 0042: The space toolbox is a layer above the engine, in `space/`

Date: 2026-11-27

## Status

Accepted.

## Context

Veach is the foundation of a reusable space-game toolbox ([0001](0001-reset.md)): planets, terrain, procgen,
ecosystems, building, multiplayer. Other people build games with the toolbox and extend it, including its procgen,
through modules and Luau. If engine code starts depending on toolbox types, the engine stops being reusable for
anything else, and toolbox concepts leak into its core. If the toolbox reaches into engine internals, it can't be
extended the way users extend the engine.

## Decision

- **Location:** a top-level `space/` directory next to `engine/`, built as the target `veach::space` (an interface
  library today; static libraries when it has sources). It links `veach::veach` and nothing below the umbrella's
  public headers. Today it holds only a README and a build stub; nothing is built in it before the research phase.
- **Direction:** the toolbox depends on the engine; **the engine never knows the toolbox exists.** Nothing under
  `engine/` may name `veach/space`, `veach::space` or `veach_space`; `tools/scripts/check_repo.py` fails if it does.
- **Generic needs move down.** When the toolbox needs something that is not space-specific (large-world coordinates,
  a job system, GPU readback, chunk streaming, a procgen graph runtime), it is built in the engine as an engine
  feature with its own spec or ADR, and the toolbox uses it.
- **Same extension model as the engine:** toolbox features are Veach modules ([0019](0019-veach-modules-and-services.md)),
  their components are reflected and Scriptable ([0026](0026-reflection-everywhere.md)), their Luau API goes through
  `@veach`, and procgen extension points are Luau hooks and scripts.
- **Games** (and `apps/`) link `veach::space` when they use the toolbox, or only `veach::veach` when they don't.

## Consequences

- The engine stays usable without the toolbox, and the layering is checked mechanically.
- Some features need a decision about where they belong; the rule is "generic goes down". That costs a little design
  each time, which is the point.
- `veach::space` is empty until the research phase answers its questions; the layering is in place before the first
  line of toolbox code.

## Alternatives considered

- **Toolbox code inside `engine/` as more areas.** Nothing would stop engine areas from depending on it, and the
  engine would carry planets into every project.
- **A separate repository.** Cleaner separation, but two repositories to keep in step while both change quickly, and
  agents lose the single tree with one gate.
- **Toolbox under `apps/`.** Apps are programs; the toolbox is a library other programs link.
