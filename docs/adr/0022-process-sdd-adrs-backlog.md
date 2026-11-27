# 0022: Process: SDD, ADRs, backlog

Date: 2026-11-27

## Status

Accepted. Carried over from the pathtracer-era series (old ADR 0018).

## Context

Agents do most of the implementation, and they work best with explicit, checkable intent. Decisions must be
findable later. TODOs scattered through code get lost unless they are tied to a tracked item. Instructions for
agents must live in one place that every tool reads.

## Decision

**Spec-Driven Development** for capabilities. Specs live in `docs/specs/<capability>/spec.md`, created from
[specs/_template.md](../specs/_template.md):

1. Write the spec (Draft).
2. Write an ADR if the spec makes an architectural decision.
3. Implement.
4. Add tests mapped to the acceptance criteria; test names carry the criterion id, as in
   `TEST_CASE("[script.AC4] ...")`.
5. Update the status (Approved → Implemented) and the links.

The dev lead writes and approves specs that don't change the product or vision.

**ADRs** live in `docs/adr/` ([README](README.md)). They are immutable once Accepted and superseded instead of
edited. **The series restarted at 0001 with the reset ([ADR 0001](0001-reset.md)).** The earlier series lives in
the `pathtracer-era` git tag (`git show pathtracer-era:docs/adr/<file>`); it is history, not a set of decisions in
force.

**Backlog** is `docs/backlog/BACKLOG.md`, with sections Now / Next / Later / Blocked / Done. Items have ids
`VCH-NNN`, a title, an area, a description and a link to their spec or ADR.

**TODO rule.** Every code TODO or FIXME references a backlog item, as in `// TODO(VCH-012): ...`.
`tools/scripts/check_repo.py` enforces it in CTest and CI: it rejects bare markers and references to ids missing
from `BACKLOG.md`.

**Slash commands.** `/adr`, `/spec` and `/backlog` (in `.claude/commands/`) create files from the templates with
the next free number or id.

**Agent instructions.**

- `AGENTS.md` is the single source of agent instructions, for any tool.
- `CLAUDE.md` contains exactly one line, `@AGENTS.md` (Claude Code's import syntax). An import, not a symlink,
  because symlinks break on Windows checkouts by default. `check_repo.py` enforces the one-line rule.

**Docs in the same commit.** A commit that changes a decision, spec or backlog item updates that file in the same
commit.

**Version control.** Work directly on `main`, in small commits in Conventional Commits style
(`feat(script): ...`, `docs(adr): ...`, `build(deps): ...`), pushed through the local gate
([ADR 0034](0034-pre-push-gate-no-gpu-runner.md)).

## Consequences

- Intent, decisions and remaining work are all text in the repo, readable by agents without external tools.
- Some ceremony per capability, kept small by templates and slash commands.
- Working on `main` with no PRs relies on the gate and CI being green. Branches and PRs come back when there are
  more contributors.
- Old ADR numbers cited anywhere outside the tag are stale; links in the docs point at the new series only.

## Alternatives considered

- **GitHub Issues or Linear for the backlog.** Invisible to an offline agent, not versioned with the code, and
  TODOs can't be checked against it in CI.
- **No specs, just ADRs.** ADRs record choices, not testable behavior.
- **Tool-specific instruction files** (`.cursorrules`, a full `CLAUDE.md`). They duplicate and drift.
- **Keeping the old ADR series and superseding its renderer ADRs one by one.** Most of it described an ended plan;
  a fresh series that states only what is in force is shorter to read, and the tag keeps the history.
