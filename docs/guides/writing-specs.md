# Writing specs

Veach uses spec-driven development for capabilities ([ADR 0018](../adr/0018-process-sdd-adrs-backlog.md)). A spec says what a
capability must do, in testable terms, before (or while) it is built.

## When you need one

- A new capability (a subsystem, a module, an API surface such as a new Luau binding family).
- A behavior change that users or agents can observe: flags, file formats, script semantics, error behavior.

Not for refactors, bug fixes that restore specified behavior, or internal changes. Decisions
(why X over Y) go in an ADR; tasks go in the backlog.

## How

1. Copy [../specs/_template.md](../specs/_template.md) to `docs/specs/<capability>/spec.md`
   (or run `/spec <capability>` in Claude Code). Use a short kebab-case folder name.
2. Fill in the sections: Summary, Motivation, Requirements (MUST / SHOULD / MAY), API sketch,
   Acceptance criteria, Out of scope, Open questions, Status, Links.
3. **Acceptance criteria are testable and numbered** `<area>.ACn` (for example `script.AC4`).
   Each says an observable outcome: "a failing system run discards only that run's writes",
   not "errors are handled well".
4. Name the test that verifies a criterion after it: `TEST_CASE("[script.AC4] ...")`. Criteria
   that need the real binary are CTest checks in `tests/CMakeLists.txt`; mention the test name in the spec.
5. Link the ADRs the spec relies on and the backlog items (`VCH-NNN`) it covers or leaves for later.

## Status flow

`Draft` → `Approved` → `Implemented`

- **Draft**: being written; open questions remain.
- **Approved**: requirements and criteria are agreed; implementation can start.
- **Implemented**: every acceptance criterion has a passing test. Update the status in the same commit as the code.

Workflow: spec → ADR if the change is architectural → implement → tests mapped to criteria → update status.

## Who approves

The dev lead writes and approves specs. A spec goes to the product owner only if it changes
the product or the vision: anything contradicting [VISION.md](../VISION.md), a non-negotiable in
[ARCHITECTURE.md](../ARCHITECTURE.md), or an accepted ADR. Bring a recommendation, not an open question.

If implementation shows a criterion was wrong, change the spec in the same commit as the code;
never leave code and spec disagreeing.
