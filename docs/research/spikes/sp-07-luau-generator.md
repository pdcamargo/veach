# SP-07: deterministic Luau generator (VCH-083)

> **Promoted (VCH-112).** The sandbox is now `engine/procgen` (`veach/procgen/sandbox.hpp`); the tests and the
> committed climate hash are unchanged. See the [procgen spec](../../specs/procgen/spec.md).

Validates the Luau procgen sandbox of [ADR 0052](../../adr/0052-procgen-runtime.md) and the Luau rules of
[ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md), on `veach::dmath`
([ADR 0067](../../adr/0067-dmath-on-llvm-libc.md)) and `veach::hash` ([ADR 0044](../../adr/0044-counter-based-hash-randomness.md)).
It also builds the interrupt and budget half of VCH-034, for generators only. Research:
[05-procgen-graph](../05-procgen-graph.md) spike S2, [12-determinism](../12-determinism.md) §8 and spike S4.

**Verdict.** The decisions hold. The fastcall bypass is real and has a second face: at optimization level 2 the
compiler also folds `math.sin(0.5)` with the host's libm. `disabledBuiltins` closes both. A climate and biome hook
in strict Luau over a 64² × 6 cube map gives the same hash in Debug and Release on macOS arm64, and the same bits as a
C++ port of it on dmath. Windows x64 is not observed yet: the hash is committed in a unit test that Windows CI runs on
the next push. A runaway hook stops with `procgen.budget_exceeded`. The hook costs 2.1 µs per cell (0.48 M cells/s,
51 ms per body). That is 2.3× the C++ port, and dmath accounts for most of the time in both.

## What was built

| File | What |
|---|---|
| `engine/script/include/veach/script/procgen_sandbox.hpp`, `src/procgen_sandbox.cpp` | `veach::procgen::Sandbox`: its own `lua_State` (not the gameplay `ScriptRuntime`), `load(source)`, `run(hook, {seed, params, inputs, outputs})`, `SandboxOptions` (bypass knob, safepoint, wall-clock and memory budgets, optimization level), `HookStats` |
| `tests/scripts/sp07_climate.luau` | The climate and biome hook, `--!strict`, checked by the luau-analyze test |
| `tests/support/sp07_climate.hpp` | Inputs: an SP-05 body and a cube map of latitude and hash-noise altitude, all canonical C++ |
| `tests/procgen_sandbox_tests.cpp` | `[sp07]` tests: bypass reproduced and closed, removed globals, `^` rejected, budgets, statelessness, `ctx.random`, the committed climate hash |
| `tests/bench/sp07_luau_generator_bench.cpp` | `veach-bench-sp07`: hashes, cells/s per variant, the C++ port, per-call overhead |
| `engine/core/include/veach/core/error_codes.inl`, `docs/reference/errors.md` | `procgen.budget_exceeded` |
| `tools/scripts/wincheck.sh` | Adds Luau's `Ast/include` (the sandbox uses Luau's parser) |

The sandbox lives in `engine/script` because `engine/procgen` doesn't exist yet and other spikes may create it. It
moves there with the stage runtime ([ADR 0065](../../adr/0065-engine-areas-for-the-toolbox.md)).

**What a hook sees.** The standard libraries without `os`, `debug`, `print`, `getfenv`, `setfenv`, `gcinfo`,
`collectgarbage`, `newproxy` and `require`. `math.random`, `math.randomseed`, `math.noise` and `vector.angle` (libm
`atan2`) are removed. The 14 libm-backed `math` functions (`sin cos tan asin acos atan atan2 exp log log10 pow sinh
cosh tanh`) are rebound to dmath; `math.log(x, b)` keeps Luau's contract (log2 and log10 paths for 2 and 10). The rest
of `math` (abs, floor, ceil, sqrt, fmod, frexp, ldexp, modf, round, min, max, clamp, sign, deg, rad, lerp, map) is
exact IEEE arithmetic or plain arithmetic in a VM built without contraction, and stays as it is. Then `luaL_sandbox`
makes every library read-only. A hook is a chunk that returns `function(ctx, inputs, outputs)`:
`ctx.params` holds named numbers, `ctx.random(a, ...)` returns `unit_double(hash(seed, a, ...))` (1 to 6 integer
words below 2^53), and `inputs` and `outputs` hold named `buffer`s. Outputs are copied back only on success.

**Statelessness.** Each call loads the chunk again into a fresh `luaL_sandboxthread`, so neither globals nor upvalues
survive from one call to the next. A test increments both, and every call sees 1. Without this, a hook could cache
across bodies and its output would depend on generation order. Reloading costs part of the 0.45 µs per-call overhead
below.

## 1. The fastcall bypass, reproduced and closed

`SandboxOptions::close_fastcall_bypass = false` keeps the same rebinding but compiles without `disabledBuiltins`. The
sandbox counts calls into its dmath functions (`HookStats::dmath_calls`).

| Case | Bypass open | Closed (`disabledBuiltins`) |
|---|---|---|
| `math.sin(i * 0.37)` in a 100-iteration loop | 0 dmath calls: `LOP_FASTCALL1` → `luauF_sin` → libm `sin`, because `luaL_sandboxthread` sets `safeenv` | 100 dmath calls |
| `math.sin(0.5)`, a constant, at O2 | 0 dmath calls; the value is the host's `std::sin(0.5)`, folded into the bytecode by the compiler (`BuiltinFolding.cpp`) | 1 dmath call, `dmath::sin(0.5)` |
| All 14 functions once (`log` twice) | — | 15 dmath calls; the sum is bit-equal to the same expression in C++ on dmath |
| By value: the first of 2·10^6 hashed x in [−100, 100] where this platform's `sin` ≠ `dmath::sin` | x = −0x1.d0528b45d178cp+4: returns Apple's 0x1.5b6df3521c35ep-1 | returns dmath's 0x1.5b6df3521c35fp-1 (1 ulp apart) |

The by-value test searches on each platform, because Apple libm and the MSVC CRT are wrong at different inputs.
The climate hook shows the bypass at scale: with the bypass open its output hash is `0xfff929faa71c0d67` instead of
`0x7e5315bcd6461c30`.

**Two more libm paths, closed by rejecting `^`.** The compiler folds `2 ^ 0.5` with the host's `pow` at
optimization level 1 and above (`ConstantFolding.cpp`), and the VM runs `luai_numpow`, a macro for `pow` in
`lnumutils.h` with no `#ifndef` guard. Pointing it at `dmath::pow` would mean patching vendored Luau, and it would
still leave compile-time folding on the host. ADR 0052 left the choice to this spike, and the sandbox rejects `^`
and `^=` at load time (`script.compile_failed`, with the line and the hint "write math.pow(x, y) … or x * x"). Gameplay
scripts keep `^`.

## 2. The climate hook and its hash

Inputs, all canonical C++. The body is the first potentially habitable SP-05 body in the galaxy's central cell
(universe seed 42) that is not tidally locked, has a tilt above 0.1 rad and has T_eq between 230 and 290 K:
`S:1000400100008000/B:1`, with T_eq 251.968 K and a tilt of 0.6734 rad (38.6°). The cube map has 64 × 64 cells on each
of 6 faces (24,576 cells). Latitude is `dmath::asin` of the normalized direction. Altitude is 4 octaves of hashed value
noise, giving 58% ocean. Greenhouse warming is a fixed 33 K.

Per cell, the hook computes:

- **Temperature:** annual-mean daily insolation over 8 seasons (`asin`, `sin`, `tan`, `acos`, `cos`), radiative
  balance via `math.pow(q, 0.25)` mixed toward the mean, then a lapse rate.
- **Precipitation:** Hadley and storm-track bands (`exp`), a Clausius-Clapeyron factor and relief, jittered by
  `ctx.random(1, i)`.
- **Biome:** a Whittaker-style id (13 classes).

The result is 242.5 to 289.5 K and up to 3,621 mm/yr, with 7 biomes present: ocean 14,297, ice sheet 569, tundra
1,447, boreal 2,767, grassland 3,365, temperate forest 1,324, temperate rainforest 756, alpine 51.

| Hash | Value | macOS arm64 Debug | macOS arm64 Release | Windows x64 (MSVC) |
|---|---|---|---|---|
| Inputs | `0xc699444c7897321c` | yes | yes | pending CI |
| Outputs | `0x7e5315bcd6461c30` | yes | yes | pending CI |
| C++ port of the hook on dmath (bench) | `0x7e5315bcd6461c30` | — | yes, bit-equal to Luau | — |

Both hashes are committed in `[sp07] the climate hook's output hash matches the committed one`, which runs in the
normal test set on the Mac gate and Windows CI. The Luau VM is compiled with `-ffp-contract=off -fno-fast-math` (the
global `add_compile_options` precedes `CPMAddPackage`; checked in `compile_commands.json` for `lbuiltins.cpp`) and with
`/fp:precise` on MSVC. That the port is bit-equal says the interpreter's arithmetic is exactly IEEE operation for
operation. The same test also checks the Windows CRT's `strtod` on the hook's decimal literals (see the hazards below).

## 3. Budgets

The interrupt callback (`lua_Callbacks::interrupt`) counts **safepoints**: loop back edges, calls and returns, C
functions included. GC steps don't count.

| Budget | Behavior | Deterministic |
|---|---|---|
| Safepoints, default 10^8 | `while true do end` with a budget of 10^6 stops at safepoint 1,000,001 with `procgen.budget_exceeded`; the message names the hook and the budget, and the hint says to look for a loop that never ends, split the work or raise the budget | yes, for a given Luau version and compile options |
| Same, wrapped in `pcall` | still stops: once over, every later safepoint raises again, so the error escapes the `pcall` | yes |
| Wall clock, default off | checked every 1,024 safepoints; a 20 ms budget stops an endless loop well inside the test's 2 s limit | no: a hang guard only |
| Memory, default 256 MiB | the allocator refuses past the cap and Luau raises a memory error; reported as `procgen.budget_exceeded` (memory); a 16 MiB cap stops an endlessly growing table | yes |

After each failure the same sandbox runs the next hook normally. The climate hook passes 2,923,074 safepoints
(119 per cell), 34× under the default budget.

Safepoint counts depend on the compiled bytecode, so a Luau upgrade or a different optimization level changes them.
Closing the bypass quadruples them, from 28 to 119 per cell, because disabled builtins become real calls. A hook near
its budget could start failing after an upgrade, so budgets need headroom and belong to the generator manifest's Luau
version.

## 4. Throughput and overhead (Release, M5 Max, one core, best of 7)

| Variant | ms per body (24,576 cells) | M cells/s | ns/cell | safepoints/cell | dmath calls/cell |
|---|---|---|---|---|---|
| **Sandbox as shipped** (dmath, interrupt, O2) | **50.7** | **0.48** | **2,063** | 118.9 | 90.9 |
| Bypass open (libm fastcalls; wrong hash) | 21.0 | 1.17 | 856 | 28.0 | 0 |
| No interrupt callback | 47.1 | 0.52 | 1,918 | 0 | 90.9 |
| Optimization level 1 | 52.3 | 0.47 | 2,128 | 126.9 | 90.9 |
| C++ port on dmath | 22.1 | 1.11 | 900 | — | — |

| Per-call overhead | µs |
|---|---|
| Empty hook, no I/O (fresh thread, chunk reload, ctx tables, call) | 0.45 |
| Empty hook with the climate I/O (4 params; 384 KiB of input copied in; 408 KiB of zeroed output allocated and copied out) | 20.4 to 21.6 |

The figures break down as follows:

- dmath costs about 10 ns per call. The C++ port spends nearly all of its 900 ns per cell in 91 dmath calls, so
  generator math, not Luau, sets the floor.
- Luau adds 1.15 µs per cell, about 13 ns for each dmath call that is now a real `LOP_CALL` instead of a fastcall,
  plus the interpreter.
- The interrupt callback costs about 7%. O2 against O1 is worth 3%.

At 51 ms per body for a hook this trig-heavy, Luau coarse maps at 64² × 6 fit the ADR's once-per-body worker job.
Fields at 10^6 samples and above do not, which matches the ADR.

Debug, for reference: 663 ms per body. The bench prints the same output hash in Debug and Release.

## Do the decisions hold?

- **ADR 0052, the Luau procgen sandbox:** holds. Every bullet is built and tested: a separate VM, no world access,
  `disabledBuiltins` with `math` rebound to dmath, no `math.random`, `math.noise` or `os.clock`, randomness only from
  the context's hash, an interrupt budget failing with `procgen.budget_exceeded`, and interpreted only. This spike
  settles the open `^` question: **`^` is rejected in generators**, and `luai_numpow` stays untouched engine-wide.
  The sandbox adds two things the ADR doesn't state, both worth writing into the implementation spec:
  - a fresh closure per invocation (statelessness);
  - a memory budget next to the instruction budget.

  The reopen condition ("Luau arithmetic isn't bit-equal across platforms") is not triggered on the Mac. The interpreter
  matches C++ bit for bit, and Windows is pending the CI run of the committed hash.
- **ADR 0043, the Luau rules:** hold. The VM inherits the strict FP flags. `disabledBuiltins` plus the remapping makes
  `math.sin(x)` canonical. The "lint-flag `^`" option becomes a hard compile error.
- **ADR 0052, the coarse-map limit (≤ ~64² × 6 in Luau):** holds at 51 ms per body for a heavy hook, which is 2.3× C++.
- **Native codegen stays off for generators.** `Luau.CodeGen` isn't linked, and the sandbox never calls
  `luau_codegen_compile`, so `--!native` is inert. When VCH-034 enables codegen for gameplay scripts, generators must
  stay interpreted until a spike shows arm64 and x64 lowerings agree bit for bit (FMA formation is the open question).

**Hazards found that the sandbox doesn't close** (rules for the implementation spec):

1. **Decimal literals go through the CRT's `strtod`** (Luau's `Parser.cpp`). Apple libc and the UCRT are both
   correctly rounded, so this is believed safe, but it formally breaks ADR 0043 rule 7. The committed hash checks it
   on the next Windows CI run.
2. **`pairs` order over tables keyed by tables, functions or userdata follows addresses** (`hashpointer` in
   `ltable.cpp`). String and number keys are deterministic: string hashes are unseeded. Generators must not let
   output depend on iteration order over reference keys.
3. **`buffer.writeu8/u16/u32` of a fractional or out-of-range number** converts with C casts whose results differ
   between platforms. Hooks must write integers in range; a checked write helper or a debug-mode check could
   enforce it.
4. **Safepoint counts are versioned with Luau** (section 3).

## Follow-ups

- Windows x64: confirm `[sp07]` passes on CI after the push. If it fails, the inputs hash shows whether the inputs or
  the hook diverged.
- Move `Sandbox` into `engine/procgen` with the stage runtime (ADR 0065), one per worker thread (VCH-015), and wire
  its errors into `procgen.stage_failed`.
- A generator type environment for luau-analyze (a `.d.luau` without `math.random`, `math.noise`, `os` and `^`, with
  `Context` typed), so mistakes show in the editor, not at load time.
- VCH-034's gameplay half: the same interrupt in `ScriptRuntime`, with a wall-clock budget per system run (gameplay
  is replay tier, so the clock is acceptable there).
- Optional defense in depth: scan compiled bytecode for `FASTCALL` of a disabled builtin id before loading.
- The memory-budget bytes are per sandbox, not per call; make them per invocation if hooks ever run concurrently in
  one VM.
- If hooks ever run per chunk, cut the I/O copy (20 µs for 790 KiB) by writing straight into Luau buffers the
  caller reads back.

## How it was measured

```sh
cmake --preset debug && cmake --build --preset debug
build/debug/tests/veach-tests -tc="*sp07*"
cmake --preset release && cmake --build --preset release --target veach-tests veach-bench-sp07
build/release/tests/veach-tests -tc="*sp07*"
build/release/tests/veach-bench-sp07          # run twice; the tables above are from both runs (within 1%)
```
