---
description: Add a backlog item with the next free VCH id
argument-hint: <title> [— area, short description]
---

Add a backlog item for: **$ARGUMENTS**

1. Read `docs/backlog/BACKLOG.md`. The new id is the highest `VCH-NNN` anywhere in the file (all sections, including Done) plus one, zero-padded to three digits. Ids are never reused.
2. Follow the conventions in `docs/backlog/README.md`: one line per item in the form
   `- **VCH-NNN** <Title> — *<area>* — <one-sentence description>. [spec](../specs/...) / [ADR](../adr/...)`
3. Put it in `Later` unless we agreed it is `Next` or `Now`. Keep `Now` small (what is actively being worked on).
4. If code needs a marker for it, write `// TODO(VCH-NNN): ...` (bare TODOs fail CI).
5. Commit with the code change it belongs to, or alone as `docs(backlog): VCH-NNN <title>`.
