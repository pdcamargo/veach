# Upstream Flecs documentation (vendored)

These files are unmodified copies of the `docs/` folder of
[SanderMertens/flecs](https://github.com/SanderMertens/flecs) at tag **v4.2.0**,
the version Veach pins in `cmake/VeachDependencies.cmake`. They are here so that
agents and humans read docs that match the code we compile, not whatever the
website shows today (the v3 → v4 API change makes stale docs actively harmful).

- Upstream is the reference for Flecs itself.
- [`../README.md`](../README.md) is the Veach-specific layer: which idioms we use,
  which traps we have hit, and what Veach code must not do.
- Images (`docs/img/`) are not vendored, so some image links in these files are broken.
- When the Flecs pin changes, re-copy these files from the new tag in the same commit.

Files: Quickstart, EntitiesComponents, Queries, Systems, ObserversManual,
ComponentTraits, Relationships, HierarchiesManual, PrefabsManual, FlecsScript,
FlecsRemoteApi, DesignWithFlecs, FAQ, MigrationGuide (v3 → v4).

## License

```
MIT License

Copyright (c) 2025 Sander Mertens
```

Flecs is distributed under the MIT license; the full text is in the Flecs
repository's `LICENSE` file and in `THIRD_PARTY_NOTICES.md` at the root of this repo.
