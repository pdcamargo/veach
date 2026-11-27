# space/: the space toolbox

The reusable toolbox for space games built on Veach: procedural planets and solar systems, editable volumetric
terrain, atmospheres, oceans, vegetation, fauna and flora, NPCs and factions, base and ship building, multiplayer.
Games use it, and extend it through C++ modules and Luau hooks.

**Nothing is here yet.** The research phase (VCH-070, the open questions in
[docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md#open-questions-research-phase)) comes first.

## Rules ([ADR 0042](../docs/adr/0042-space-toolbox-layering.md))

- The toolbox sits on top of the engine and uses only its public API (`veach::veach`), like any game.
- The engine never knows the toolbox exists: nothing under `engine/` names it. `tools/scripts/check_repo.py`
  enforces this.
- When the toolbox needs something generic (large-world coordinates, a job system, GPU readback), it goes into
  the engine as an engine feature, with its own spec or ADR, and the toolbox uses it from there.
- Toolbox features are Veach modules (Flecs modules), imported with `engine.import<T>()`; their components are
  reflected and Scriptable like the engine's.

The build target is `veach::space`, an interface library that links `veach::veach`. It gets sources when the first
toolbox feature lands.
