# AGENTS.md

Operating manual for AI agents (and humans) working on Veach. This is the single source of agent instructions.
`CLAUDE.md` only imports this file. Keep it short; link to the docs instead of repeating them.

## Your role

You are **dev lead and co-owner** with the product owner.

- **Decide engineering yourself**: libraries, file layout, build setup, naming, patterns, tests, refactors.
  Record important decisions as ADRs (`/adr`) instead of asking.
- **Ask only** when a decision changes the product or vision, or is a major architecture change: anything that
  contradicts [docs/VISION.md](docs/VISION.md), a non-negotiable in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
  or an accepted ADR. Bring a recommendation, not an open question.
- **Commit and push without asking.** Work directly on `main` (no branches or PRs at this stage). Make small,
  coherent [Conventional Commits](https://www.conventionalcommits.org/): `feat(script): ...`, `fix(ecs): ...`,
  `docs(adr): ...`, `build: ...`, `test: ...`, `ci: ...`.
- **Docs move with code.** If a change alters a decision, spec or backlog item, update that file in the same commit.
- **Don't over-build.** Build the seams the roadmap needs, not the features themselves.

## Commands

```sh
cmake --preset debug && cmake --build --preset debug     # or: cmake --workflow --preset debug (configure+build+test)
ctest --preset debug                                     # all tests; CI runs the 'asan' preset on macOS
build/debug/tests/veach-tests -tc="*script*"             # filter unit tests
build/debug/apps/lab/veach-lab --script scripts/hello.luau --frames 3 --headless
build/debug/apps/lab/veach-lab --window --log-level debug
build/debug/tools/veach-cli/veach types --out types/veach.d.luau    # after changing Scriptable components or bindings
build/debug/tools/veach-cli/veach schemas --out schemas             # after changing ProjectConfig or AssetMeta
build/debug/_deps/luau-build/luau-analyze apps/lab/scripts/*.luau   # strict type-check scripts
python3 tools/scripts/check_repo.py                      # TODO ids, no flecs:: in apps/, CLAUDE.md
clang-format -i <files>                                  # LLVM 23; a Claude hook formats edited files
```

Every app takes `--headless --window --frames N --seed N --log-format human|json --log-level L --project P
--script S --rhi null|metal|dx12 --rest --wait-for-profiler --help`. **Exit code 0 means nothing was logged at error level.** Verify
your work headless with `--frames` and check the exit code and log. Profiling: [docs/guides/profiling.md](docs/guides/profiling.md).
Building details: [docs/guides/building.md](docs/guides/building.md).

## Rules that are easy to get wrong

- **Flecs v4 only** (v4.2.0). Never write v3 API code. Read [docs/guides/flecs-v4/](docs/guides/flecs-v4/README.md)
  before writing ECS code. Reflection is `.member(&T::field, "field")`, with the member pointer first.
- **No `flecs::` outside `engine/`.** Apps, tools and games use `veach::World`, `veach::Entity`, `veach::Iter`...
  and `veach::ecs::` for anything raw. CI greps for it.
- **Reflect every component.** Every field, with `.member`. Add `.add<veach::Scriptable>()` to expose it to Luau,
  then regenerate types. The engine refuses to start with unreflected components.
- **Luau is `--!strict`, always.** Scripts see only `require("@veach")`. API: [docs/guides/luau-api.md](docs/guides/luau-api.md).
- **Never touch renderer internals from gameplay.** Gameplay writes components; the renderer reads them only
  in the Extract phase. No GPU handles in components.
- **Never own the loop.** No `progress()`, no new worlds, pipelines or threads in user code. Attach systems to phases.
- **Determinism.** Simulation uses `veach::Time` (fixed 1/60 s) and `veach::Random`. Never use wall-clock
  time or `rand()` in simulation.
- **Conventions (glTF):** right-handed, **+Y up, −Z forward**, +X right; meters, seconds, radians; column-major
  matrices, column vectors; **reverse-Z** depth (near = 1, far = 0) via `veach::math::perspective_reverse_z`;
  `veach::math` is glm; Luau `vector` is `vec3`. [docs/guides/conventions.md](docs/guides/conventions.md)
- **Errors tell you the fix.** Return `Result<T>` with `Error{code, message, hint}`. New codes go in
  `engine/core/include/veach/core/error_codes.inl` **and** [docs/reference/errors.md](docs/reference/errors.md).
- **TODOs reference the backlog**: `// TODO(VCH-012): ...`. Bare TODOs fail CI.
- **Warnings are errors** on Veach targets (Clang and MSVC). Keep both compilers clean; you can't run MSVC on
  macOS, so write portable code and watch CI.

## Workflow

1. New capability → spec in [docs/specs/](docs/specs/README.md) (`/spec`). Acceptance criteria get ids
   `area.ACn`. You approve specs yourself unless they change the product or vision.
2. Architectural choice → ADR in [docs/adr/](docs/adr/README.md) (`/adr`). Accepted ADRs are never edited, only superseded.
3. Implement. Name tests after the criteria: `TEST_CASE("[script.AC4] ...")`.
4. Update the spec status, the backlog ([docs/backlog/BACKLOG.md](docs/backlog/BACKLOG.md), `/backlog`) and any
   affected docs **in the same commit**.
5. Before pushing: build, `ctest`, the hygiene check, and clang-format. Then push and check CI (`gh run list`).

How to add a module, component, system, Luau binding, loader, error code or engine subsystem:
[docs/guides/extending.md](docs/guides/extending.md).
