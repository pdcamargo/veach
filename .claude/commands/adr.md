---
description: Create a new ADR from the template with the next free number
argument-hint: <short title of the decision>
---

Create an Architecture Decision Record for: **$ARGUMENTS**

1. List `docs/adr/`. The new number is the highest existing `NNNN-` prefix plus one, zero-padded to four digits (`0000` is the template, never reuse a number).
2. Copy `docs/adr/0000-template.md` to `docs/adr/NNNN-<kebab-case-title>.md`.
3. Fill every section from what we decided in this conversation: Status, Date (today), Context, Decision, Consequences, Alternatives considered. Be concrete: name files, types, flags. Status is `Accepted` when the decision is within the dev lead's authority (see AGENTS.md), otherwise `Proposed` and say in the summary that it needs the owner's call.
4. If it replaces an earlier ADR, set the old one's Status to `Superseded by NNNN` (that is the only edit allowed to an accepted ADR) and link both ways.
5. Add a row to the index table in `docs/adr/README.md`.
6. Update any doc that states the old decision (ARCHITECTURE.md, AGENTS.md, specs) in the same commit.
7. Commit: `docs(adr): NNNN <title>`.
