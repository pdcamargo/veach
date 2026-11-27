# Veach

**An AI-native C++ game engine: the foundation of a toolbox for space games.**

Veach (after Eric Veach, whose thesis underlies modern path tracing) is the engine under a reusable, extensible
toolbox for space games in the spirit of No Man's Sky: procedural planets and solar systems, editable terrain,
atmospheres, ecosystems, building and multiplayer, which other people build games with and extend. AI agents are
first-class users. Agents and humans share one API: text formats, reflection everywhere, strictly typed Luau
scripting, actionable errors, and deterministic headless runs that verify their own work.

> **Status:** the engine foundation is done; the next milestone is the research phase. The engine imports glTF
> (cooked to BC7/KTX2), runs Luau gameplay with input actions, transforms and hot reload, and renders on Metal and
> DX12 through a render graph: a basic real-time rasterizer (an interim path until the real PBR renderer is designed)
> and photo mode, a progressive path tracer with extendable Slang materials. Agents can drive any app over MCP
> (`--mcp`). The toolbox itself (`space/`) is empty until the research phase answers how planets, terrain and procgen
> are built. See the [roadmap](docs/VISION.md#roadmap) and [backlog](docs/backlog/BACKLOG.md). The earlier,
> raytracing-first plan is preserved in the `pathtracer-era` git tag.

## Quick start

Requirements: CMake ≥ 3.29, Ninja, Xcode (macOS, Apple silicon; it ships the Metal shader compiler), Git LFS, or
Visual Studio 2022 (Windows, from a Developer prompt). GPU: any Apple silicon Mac, or a DX12 GPU with Shader Model
6.6; ray tracing is optional (photo mode only). Dependencies are fetched and pinned automatically.

```sh
cmake --workflow --preset debug                 # configure, build, run all tests
build/debug/apps/lab/veach-lab --script scripts/hello.luau --frames 3 --headless
build/debug/apps/lab/veach-lab --window         # a window, driven by the Window component
build/debug/apps/lab/veach-lab --window --rhi metal --script scripts/flight_helmet_realtime.luau   # a glTF scene in real time
build/debug/apps/lab/veach-lab --window --rhi metal --script scripts/cornell_box.luau             # photo mode
```

```
[    0.010] info  script: Hello from Luau — frame 1
[    0.010] info  script: Hello from Luau — frame 2
[    0.010] info  script: Hello from Luau — frame 3
```

The script behind that:

```lua
--!strict
local veach = require("@veach")

local Greeting = veach.component("Greeting", { text = "" })

veach.system("Greet", "Simulation", veach.query(Greeting), function(_entity, greeting)
	veach.log.info(`{greeting.text} — frame {veach.time.frame()}`)
end)

veach.spawn("greeter"):set(Greeting, { text = "Hello from Luau" })
```

And a C++ module, using one header and only `veach::` names:

```cpp
#include <veach/veach.hpp>

struct Spinner { float radians_per_second = 1.0f; float angle = 0.0f; };

struct GameModule {
    explicit GameModule(veach::World& world) {
        world.module<GameModule>();
        world.component<Spinner>()
            .member(&Spinner::radians_per_second, "radians_per_second")
            .member(&Spinner::angle, "angle");
        world.system<Spinner>("Spin").kind<veach::phase::Simulation>().each(
            [](veach::Iter& it, std::size_t, Spinner& s) { s.angle += s.radians_per_second * it.delta_time(); });
    }
};

int main(int argc, char** argv) {
    veach::Engine engine({.name = "my-game"}, argc, argv);
    engine.import<GameModule>();
    return engine.run();
}
```

## Documentation

- [Vision](docs/VISION.md): what Veach is and is not.
- [The space toolbox](space/README.md): where it lives and its rules.
- [Architecture](docs/ARCHITECTURE.md): layers, non-negotiables, init order, data flow.
- [Decisions (ADRs)](docs/adr/README.md), [specs](docs/specs/README.md), [backlog](docs/backlog/BACKLOG.md).
- Guides: [building](docs/guides/building.md), [Luau API](docs/guides/luau-api.md),
  [extending the engine](docs/guides/extending.md), [conventions](docs/guides/conventions.md),
  [rendering](docs/guides/rendering.md), [profiling](docs/guides/profiling.md),
  [Flecs v4 notes](docs/guides/flecs-v4/README.md).
- [Error reference](docs/reference/errors.md), [RHI capabilities](docs/reference/rhi-capabilities.md).
- Working on Veach with an AI agent: [AGENTS.md](AGENTS.md).

## Built with

[Flecs](https://github.com/SanderMertens/flecs) (ECS), [Luau](https://luau.org) (scripting),
[SDL3](https://libsdl.org) (windowing and input), [glm](https://github.com/g-truc/glm),
[nlohmann/json](https://github.com/nlohmann/json), [Tracy](https://github.com/wolfpld/tracy) and
[doctest](https://github.com/doctest/doctest), [Slang](https://shader-slang.org) (shaders, build time),
[fastgltf](https://github.com/spnda/fastgltf), [meshoptimizer](https://github.com/zeux/meshoptimizer),
[bc7enc](https://github.com/richgel999/bc7enc_rdo), [stb](https://github.com/nothings/stb) and
[NVIDIA FLIP](https://github.com/NVlabs/flip). Licenses:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

MIT. See [LICENSE](LICENSE).
