# Metal compiler crash reproductions

Kernels that crash Metal's shader compiler service when a compute pipeline is created from them: the call fails
with `XPC_ERROR_CONNECTION_INTERRUPTED ... This error occurred after multiple retries`. Each directory holds a
standalone `kernel.metal` (Slang's Metal output of a Veach shader, as the build compiles it), the Slang `source.slang`
it came from, and `FUNCTION`, the kernel to create.

Found on:

| | |
|---|---|
| Mac | Apple M5 Max (32-core GPU), 36 GB |
| macOS | 26.3 (25D2125) |
| Xcode | 26.6 (17F113), Apple metal 32023.883 |
| Slang | 2026.22 (Veach's pinned version) |

## Running them

```sh
tools/metal-repros/run_all.sh          # every repro; exits 0 while all still crash
tools/metal-repros/build.sh tools/metal-repros/<repro> <function>   # one: metal -> metallib -> pipeline
```

`build.sh` compiles like Veach's build (`metal -std=metal3.1 -mmacosx-version-min=14.0`, then `metallib`), and
`repro.swift` creates the pipeline. Compiling to AIR succeeds; the crash is in the GPU compiler behind
`makeComputePipelineState`. Each crash reproduced on every attempt. To file one with Apple (Feedback Assistant,
Developer Tools > Metal), attach its directory.

## The repros, and what Veach does instead

| Repro | What triggers it | Workaround in Veach |
|---|---|---|
| `path-tracer-second-trace` | Photo mode's path tracer (`trace_pixels`) with a second closest-hit `trace()` call site, as a function, `[noinline]` or not | The path tracer keeps its only `trace()` call in its main loop (`engine/render/shaders/path_tracer.slang`) |

Earlier, `veach/scene.slang`'s comment on `trace()` records another case: an inlined candidate loop for the closest
hit crashed the compiler (2026-11, not kept).

## When a new Xcode ships

Run `run_all.sh`. If a repro's pipeline is created, rebuild Veach with the workaround undone (the table above),
check it compiles and the tests pass, then merge the split kernel back and delete the repro.
