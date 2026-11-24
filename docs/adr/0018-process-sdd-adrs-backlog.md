# 0018: Process: SDD, ADRs, backlog

Date: 2026-11-24

## Status

Accepted

## Context

Agents do most of the implementation, and agents work best with explicit, checkable intent. Decisions must be findable later. TODOs scattered through code get lost unless they are tied to a tracked item. Instructions for agents must live in one place that every tool reads.

## Decision

**Spec-Driven Development** for capabilities. Specs live in `docs/specs/<capability>/spec.md`, created from [specs/_template.md](../specs/_template.md). The workflow:

1. Write the spec (Draft).
2. Write an ADR if the spec makes an architectural decision.
3. Implement.
4. Add tests mapped to the acceptance criteria. Test names carry the criterion id, for example `TEST_CASE("[script.AC4] ...")`.
5. Update the status (Approved → Implemented) and the links.

Specs that don't change the product or vision are written and approved by the dev lead.

**ADRs** live in `docs/adr/` ([README](README.md)). They are immutable once Accepted and superseded instead of edited.

**Backlog** is `docs/backlog/BACKLOG.md`, with sections Now / Next / Later / Done. Items have ids `VCH-NNN`, a title, an area, a description and a link to their spec or ADR.

**TODO rule.** Every code TODO or FIXME references a backlog item, as in `// TODO(VCH-012): ...`. `tools/scripts/check_repo.py` enforces it in CTest and CI. It rejects bare markers and references to ids missing from `BACKLOG.md`.

**Slash commands.** `/adr`, `/spec` and `/backlog` (in `.claude/commands/`) create files from the templates with the next free number or id.

**Agent instructions.**

- `AGENTS.md` is the single source of agent instructions, for any tool.
- `CLAUDE.md` contains exactly one line, `@AGENTS.md`, which is Claude Code's import syntax. We use an import, not a symlink, because symlinks break on Windows checkouts by default. `check_repo.py` enforces the one-line rule.

**Docs in the same commit.** A commit that changes a decision, spec or backlog item updates that file in the same commit.

**Version control.** At this stage we work directly on `main`, with small commits in Conventional Commits style (`feat(script): ...`, `docs(adr): ...`, `build(deps): ...`).

## Consequences

- Intent, decisions and remaining work are all text in the repo, readable by agents without external tools.
- Some ceremony per capability. It is kept small by templates and slash commands.
- Working on `main` with no PRs relies on CI being green. Branches and PRs come back when there are more contributors.

## Alternatives considered

- **GitHub Issues or Linear for the backlog.** Invisible to an offline agent, not versioned with the code, and TODOs can't be checked against it in CI.
- **No specs, just ADRs.** ADRs record choices, not testable behavior. Specs give acceptance criteria for tests to map to.
- **Tool-specific instruction files** (`.cursorrules`, a full `CLAUDE.md`). They duplicate and drift. AGENTS.md is the cross-tool convention.
