---
description: Create a new capability spec (SDD) from the template
argument-hint: <capability name, e.g. "hot reload">
---

Create a spec for the capability: **$ARGUMENTS**

1. Pick a kebab-case folder name. Check `docs/specs/` so it does not already exist (if it does, update that spec instead).
2. Copy `docs/specs/_template.md` to `docs/specs/<name>/spec.md`.
3. Fill it in following `docs/guides/writing-specs.md`:
   - Requirements use MUST / SHOULD / MAY.
   - Every acceptance criterion is testable and gets an id `<area>.ACn`; the tests that cover it will be named `[<area>.ACn] ...`.
   - Status starts as `Draft`. Mark it `Approved` yourself unless it changes the product or the vision (then ask the owner, with a recommendation).
   - Link the ADRs it depends on and the backlog items (`VCH-NNN`) it implements.
4. If it needs an architectural decision, create the ADR too (`/adr`).
5. Add or update the backlog item in `docs/backlog/BACKLOG.md` to link the spec.
6. Add the spec to the list in `docs/specs/README.md`.
7. Commit: `docs(spec): <name>`.
