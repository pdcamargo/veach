# AGENTS.md

Operating manual for AI agents (and humans) working on Veach. This is the single source of agent instructions.
`CLAUDE.md` only imports this file. Keep it short; link to the docs instead of repeating them.

Veach is an AI-native C++ engine and the foundation of a space-game toolbox ([docs/VISION.md](docs/VISION.md)).
The earlier raytracing-first era (real-time GI, denoiser, upscalers) lives only in the `pathtracer-era` git tag
([ADR 0001](docs/adr/0001-reset.md)). Consult it only when the owner asks, or when reusing photo-mode internals.

## Your role

The **owner** sets product direction and does not write code. You are the **dev lead**: you own every engineering
decision and the code.

- **Decide engineering yourself**: libraries, file layout, build setup, naming, patterns, tests, refactors,
  rendering techniques, data structures. Never ask the owner to choose between engineering options you can evaluate
  yourself: evaluate them, decide, record an ADR (`/adr`) when it is architectural, and report what you decided.
- **Ask only** when a decision changes the product or vision: anything that contradicts
  [docs/VISION.md](docs/VISION.md) or a non-negotiable in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Bring a
  recommendation, not an open question. An accepted ADR that is wrong for engineering reasons is yours to supersede.
- **Commit and push without asking.** Work directly on `main` (no branches or PRs at this stage). Make small,
  coherent [Conventional Commits](https://www.conventionalcommits.org/): `feat(script): ...`, `fix(ecs): ...`,
  `docs(adr): ...`, `build: ...`, `test: ...`, `ci: ...`.
- **Docs move with code.** If a change alters a decision, spec or backlog item, update that file in the same commit.
- **Don't over-build.** Build the seams the roadmap needs, not the features themselves.
- **Ask before installing anything outside the repo** (cargo/brew/pip installs, SDKs, anything in `~` or `/usr`)
  **and before accepting any license, EULA or terms** on the owner's behalf. Inside the repo (dependencies pinned
  in CMake, `build/`) is yours. What is installed and why: [docs/guides/local-tools.md](docs/guides/local-tools.md).
- **Never estimate work in hours, days or weeks**, anywhere: summaries, ADRs, specs, review packets. Describe size
  by scope (what has to change), and bound open-ended work by attempts (measurement and tuning rounds), not time.
- **Review packets, only for product decisions.** When something changes the product or vision and needs the owner's
  decision, end the summary with a packet they can paste to another reviewer who has seen neither the repo nor the
  session. Never for engineering choices. Under ~800 words, in this shape:

  ```markdown
  ## Review packet: <title>
  - Decision needed: one sentence.
  - Context: what this is and why it matters now, in plain language; define jargon the first time it appears.
  - Options: 2-4 realistic ones, each with what it is, its tradeoffs, the measured numbers (ms, FLIP,
    complexity) and what it locks us into.
  - Recommendation: which and why, and what would change your mind.
  - Risks and reversibility: what is expensive to undo later.
  - Evidence: the key numbers and tables inline, plus file paths and commit hashes for reference.
  - Questions for the owner: specific and answerable (pick A/B/C, yes/no, set a number).
  ```

  Don't start work that depends on the decision until the owner replies.

## Commands

```sh
cmake --preset debug && cmake --build --preset debug     # or: cmake --workflow --preset debug (configure+build+test)
ctest --preset debug                                     # all tests; CI also runs 'asan' and 'leaks' on macOS
build/debug/tests/veach-tests -tc="*script*"             # filter unit tests
build/debug/apps/lab/veach-lab --script scripts/hello.luau --frames 3 --headless
build/debug/apps/lab/veach-lab --window --log-level debug
build/debug/tools/veach-cli/veach types --out types/veach.d.luau    # after changing Scriptable components or bindings
build/debug/tools/veach-cli/veach schemas --out schemas             # after changing ProjectConfig or AssetMeta
build/debug/_deps/luau-build/luau-analyze apps/lab/scripts/*.luau   # strict type-check scripts
python3 tools/scripts/check_repo.py                      # TODO ids, no flecs:: in apps/, CLAUDE.md
build/debug/apps/lab/veach-lab --rhi metal --headless --frames 3 --script scripts/flight_helmet_realtime.luau --screenshot out.png
build/debug/tools/veach-cli/veach imgdiff tests/golden/flight_helmet_realtime.png out.png   # perceptual (FLIP) compare
VEACH_DISABLE_RAY_TRACING=1 build/debug/apps/lab/veach-lab --rhi metal ...   # prove a change needs no ray tracing
cmake --build --preset debug --target update-goldens     # regenerate golden images ON PURPOSE, then inspect them
clang-format -i <files>                                  # LLVM 23; a Claude hook formats edited files
```

Every app takes `--headless --window --frames N --seed N --log-format human|json --log-level L --project P
--script S --rhi null|metal|dx12 --rest --wait-for-profiler --screenshot PATH --write-types PATH --mcp
--record PATH --replay PATH --help`. A recording repeats a run exactly, headless: record a bug once with `--record`, then
`--replay` it as often as needed ([spec](docs/specs/record-replay/spec.md)).
**Exit code 0 means nothing was logged at error level**, GPU validation and GPU errors included. Verify
your work headless with `--frames` and check the exit code and log. Profiling: [docs/guides/profiling.md](docs/guides/profiling.md).
Building details: [docs/guides/building.md](docs/guides/building.md).

## Driving the engine over MCP

`--mcp` turns any Veach app into an MCP server on stdio ([spec](docs/specs/mcp-server/spec.md)): query and edit
components, spawn entities, step frames, take screenshots, read the render-graph plan and each pass's GPU time
(`gpu_timings`), render a path-traced reference image (`photo_mode`) and run Luau in the live world. The repo's [.mcp.json](.mcp.json) registers the lab as
`veach-lab` (headless, Metal), so Claude Code offers it once the debug build exists; approve it when asked, or
check with `claude mcp list`. To add it by hand or with other flags:

```sh
claude mcp add veach-lab -- build/debug/apps/lab/veach-lab --mcp --headless --rhi metal
```

Headless sessions are deterministic: nothing moves until you call `step_frames`. Press keys with `send_input`
(they drive the actions bound in `veach.project.json`). After editing a script or asset, call `reload` (headless
runs never reload by themselves; windowed runs do, every 250 ms), then `list_assets` to see errors. For ground
truth, `photo_mode { samples }` renders the camera's view with the reference path tracer (it needs a `PhotoMode` on
the camera and a GPU with ray tracing; see `apps/lab/scripts/cornell_box.luau`). Drop `--headless` to poke at a
running window. Look at screenshots you take; a successful call is not a correct image.

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
- **Rendering goes through the render graph.** Declare every resource a pass touches; never issue barriers by
  hand. Shaders are Slang only, and resources are reached through `DescriptorHandle`s
  ([docs/guides/rendering.md](docs/guides/rendering.md)). Check visual changes with `--screenshot` and look at
  the image. Golden tests skip on GPUs without Metal 3 (photo-mode ones also without ray tracing), so a skip is
  not a pass.
- **The real-time renderer is raster.** No real-time GI or path tracing, and no real-time feature may require
  hardware ray tracing; only photo mode uses it ([ADR 0003](docs/adr/0003-platforms-and-hardware-target.md),
  [ADR 0039](docs/adr/0039-realtime-renderer-is-raster-pbr.md)). Today's real-time path is interim
  ([ADR 0040](docs/adr/0040-interim-realtime-path-visibility-buffer.md)): don't polish it; the planned renderer is
  [ADR 0060](docs/adr/0060-gpu-driven-visibility-buffer.md).
- **The engine never knows the toolbox.** `space/` builds on `veach::veach`; nothing under `engine/` may name it
  (CI checks). A generic need of the toolbox becomes an engine feature ([ADR 0042](docs/adr/0042-space-toolbox-layering.md)).
- **The toolbox architecture is decided; spikes come before features.** Read
  [ARCHITECTURE.md](docs/ARCHITECTURE.md#toolbox-architecture-decided-in-the-research-phase) and [docs/research/00-synthesis.md](docs/research/00-synthesis.md)
  before toolbox work. Validate a decision with its spike (VCH-077 to VCH-097) before building on it. What the GPU
  layer can do: [docs/reference/rhi-capabilities.md](docs/reference/rhi-capabilities.md).
- **Determinism has tiers** ([ADR 0043](docs/adr/0043-determinism-tiers-and-strict-floating-point.md)): canonical
  code (seeds, generation records, orbits, terrain density, edits, generator hooks) must be bit-identical on Mac and
  Windows, uses `veach::dmath` and counter-based hashing, never platform `sin`/`exp` or a shared random stream; GPU
  output is visual-only and never feeds gameplay.
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
- **Metal's compiler crashes on some large kernels** (`XPC_ERROR_CONNECTION_INTERRUPTED` creating the pipeline).
  When you split a kernel to avoid one, save a standalone repro in [tools/metal-repros/](tools/metal-repros/README.md);
  run them after each Xcode update and merge the split back once one passes.
- **Windows is CI-only** ([ADR 0032](docs/adr/0032-windows-verified-in-hosted-ci.md)). Nobody has a Windows machine:
  never plan work that needs a person on Windows. Windows (DX12 on WARP included) is verified by GitHub-hosted CI;
  real GPU hardware only on macOS. Keep Windows-touching commits small so a red job points at one change. If
  something can't be verified without a Windows machine, add it to the backlog as **Blocked** and move on.

## Workflow

1. New capability → spec in [docs/specs/](docs/specs/README.md) (`/spec`). Acceptance criteria get ids
   `area.ACn`. You approve specs yourself unless they change the product or vision.
2. Architectural choice → ADR in [docs/adr/](docs/adr/README.md) (`/adr`). Accepted ADRs are never edited, only superseded.
3. Implement. Name tests after the criteria: `TEST_CASE("[script.AC4] ...")`.
4. Update the spec status, the backlog ([docs/backlog/BACKLOG.md](docs/backlog/BACKLOG.md), `/backlog`) and any
   affected docs **in the same commit**.
5. Push with `tools/push.sh`: it runs the local gate (`tools/scripts/pre-push.sh`: the build, the tests with GPU
   goldens required on macOS, the hygiene check and clang-format), then `git push`, so no connection sits idle during
   the gate. The fast test set by default; the full set, which adds the tests labeled `full`
   (Bistro and other heavy GPU tests), when the pushed commits touch rendering code, or with `--full`. Run the full
   set (`VEACH_TESTS=full tools/scripts/pre-push.sh`) before any milestone summary. The same gate is the git hook
   (`git config core.hooksPath tools/githooks`) for a plain `git push`.
   Then check CI (`gh run list`). CI has no real Apple GPU, so this gate is what verifies Metal
   ([ADR 0034](docs/adr/0034-pre-push-gate-no-gpu-runner.md)): never push around it. Docs-only pushes skip the
   build jobs.

How to add a module, component, system, Luau binding, loader, error code or engine subsystem:
[docs/guides/extending.md](docs/guides/extending.md).
