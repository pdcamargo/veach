# Backlog conventions

The backlog is [BACKLOG.md](BACKLOG.md), in the repo, next to the code it describes. No external tracker.

## Sections

- **Now**: actively being worked on. Keep it short (one to three items).
- **Next**: agreed to be done soon, roughly in order.
- **Later**: the roadmap. Order is loose.
- **Blocked**: cannot be done or verified with what we have (for example real Windows GPU hardware, [ADR 0032](../adr/0032-windows-verified-in-hosted-ci.md)). Each says what would unblock it.
- **Done**: finished items, newest first. Never delete an item; move it here, including dropped ones (say so). The one exception was the reset ([ADR 0001](../adr/0001-reset.md)), which removed the items of the ended plan; they live in the `pathtracer-era` tag.

## Items

One line per item:

```
- **VCH-NNN** Title — *area* — One-sentence description. [spec](../specs/<name>/spec.md) / [ADR](../adr/NNNN-....md)
```

- Ids are `VCH-` plus three or more digits, assigned in increasing order (`/backlog` picks the next one). Ids are never reused or renumbered, because code and commits refer to them.
- **Area** is one of: `core`, `ecs`, `platform`, `asset`, `script`, `rhi`, `render`, `runtime`, `tools`, `agents`, `physics`, `audio`, `ui`, `editor`, `space`, `process`.
- Link the spec or ADR when one exists. Big items get a spec before implementation starts (see [writing specs](../guides/writing-specs.md)).

## TODOs in code

Every TODO, FIXME, XXX or HACK in code must reference a backlog item:

```cpp
// TODO(VCH-012): create the ID3D12Device.
```

`tools/scripts/check_repo.py` (run by CTest and CI) rejects bare TODOs and TODOs whose id is not in BACKLOG.md.

## Keeping it in sync

When a commit finishes, starts or changes the scope of an item, update BACKLOG.md in the same commit.
