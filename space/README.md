# space/: the space toolbox

The reusable toolbox for space games built on Veach: procedural planets and solar systems, editable volumetric
terrain, atmospheres, oceans, vegetation, fauna and flora, NPCs and factions, base and ship building, multiplayer.
Games use it, and extend it through C++ modules and Luau hooks.

**What is here today is spike code** ([docs/research/spikes/](../docs/research/spikes/)), throwaway prototypes that
validated the architecture ([docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md#toolbox-architecture-decided-in-the-research-phase),
[docs/research/00-synthesis.md](../docs/research/00-synthesis.md), ADRs 0043-0069): the universe clock and bodies on
rails (SP-04, `src/`, `include/`) and the system generation stages (SP-05, `gen/`). `SpaceMcpModule`
(`mcp_tools.hpp`) exposes both to agents as MCP tools, `orrery_state` and `generate_system` (VCH-100). The real toolbox replaces them as
the spikes toward the first slice (VCH-082 to VCH-091) land.

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
