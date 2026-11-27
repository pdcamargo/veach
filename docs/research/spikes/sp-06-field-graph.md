# SP-06: one field graph, three evaluators (VCH-082)

Validates [ADR 0052](../../adr/0052-procgen-runtime.md) (JSON field graphs, a canonical CPU batch interpreter, a
visual-only GPU interpreter) with spike S1 of [research 05](../05-procgen-graph.md#12-spike-candidates), under the
determinism rules of [ADR 0043](../../adr/0043-determinism-tiers-and-strict-floating-point.md) and the hash of
[ADR 0044](../../adr/0044-counter-based-hash-randomness.md).

**Result: the decision holds (D1, interpreters).** All three evaluators agree within **5.7e-7** on Metal (the target is
1e-5). The GPU interpreter costs **1.28×** the hand-written kernel (the target is under 2×). The CPU batch interpreter
reaches **20 M samples/s on one core** (the target is 10 M). Batches give the same bits as single samples, the same in
Debug and Release, and the same on x86-64 without FMA as on arm64 with it. Compiling graphs to Slang at run time (D3,
VCH-009) is not needed for speed.

The spike found one thing the ADRs don't say, and it matters more than the interpreter's overhead: **the GPU's fast
math fuses multiply-adds, and in gradient noise two of them cancel badly.** Left alone, CPU and GPU differ by up to
9.7e-6 on this graph (just under the target), and the difference grows with frequency (9e-4 for one octave at
frequency 2,560). Computing those two operations as one correctly rounded fused multiply-add on every evaluator brings
it down to 5.7e-7 at every frequency tested. Section 3 has the details.

WARP is not measured yet: the GPU test runs on Windows CI, which has not run this branch.

## What was built

All throwaway, under `tests/sp06/` (a static library `veach-sp06`, shared by the unit tests and the bench so the
shaders compile once). Nothing was added to `engine/`: ADR 0065 puts the CPU interpreter in `engine/procgen` and the
GPU half in `engine/render`, and both should start from a reviewed node set, not from this spike.

| Piece | Where | Notes |
|---|---|---|
| The graph | `tests/sp06/terrain.field.json` | 8 nodes, 11 noise octaves per sample |
| Compiler: JSON → op list | `tests/sp06/field_graph.cpp` `compile()` | Checks types, versions, ports, units and cycles; drops dead nodes; folds constants; allocates registers; derives octave seeds |
| CPU batch interpreter | `field_graph.cpp` `evaluate_batch()` / `evaluate_scalar()` | Each op runs over a batch of 256 samples; plain loops that Clang auto-vectorizes (NEON, 4 wide) |
| GPU interpreter | `tests/sp06/shaders/sp06_field_interpreter.slang` | Walks the op list from a `StructuredBuffer<uint>`, 8 float registers |
| Hand-written kernel | `tests/sp06/shaders/sp06_field_handwritten.slang` | The same graph with every constant and seed baked in (what D3 would generate) |
| Shared noise | `tests/sp06/shaders/sp06_field_noise.slang` | Used by both kernels; mirrors the C++ line by line |
| GPU runner | `tests/sp06/field_gpu.cpp` | Both kernels as render-graph compute passes, timed per pass (VCH-049); `parameters_match` checks the C++ mirror of the parameter block against both shaders |
| Tests `[sp06.AC1-3]` | `tests/sp06/sp06_field_graph_tests.cpp` | In `veach-tests`; the GPU case skips without a GPU like the goldens |
| `veach-bench-sp06` | `tests/sp06/sp06_field_graph_bench.cpp` | The tables below and the image; `--graph` evaluates another graph on the CPU and the GPU interpreter |

CMake: one line in `tests/CMakeLists.txt` (`add_subdirectory(sp06)`); the rest is in `tests/sp06/CMakeLists.txt`.

### The IR

```json
{
  "veach_graph": 1, "id": "sp06:terrain",
  "inputs": { "p": "p_planet" },
  "nodes": [
    { "id": "continents", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "planet_m": 3000000 }, "octaves": 6, "lacunarity": 2.0, "gain": 0.5 } },
    { "id": "mountains", "type": "veach:fbm@1", "inputs": { "p": "p" },
      "params": { "wavelength": { "planet_m": 600000 }, "octaves": 5 } },
    { "id": "ridges", "type": "veach:ridge@1", "inputs": { "x": "mountains" }, "params": { "offset": 1.0 } },
    { "id": "peaks", "type": "veach:remap@1", "inputs": { "x": "ridges" }, "params": { "from": [0, 1], "to": [0.35, 1] } },
    { "id": "land", "type": "veach:remap@1", "inputs": { "x": "continents" }, "params": { "from": [-1, 1], "to": [0, 0.6] } },
    { "id": "mask_raw", "type": "veach:remap@1", "inputs": { "x": "continents" }, "params": { "from": [0.05, 0.3], "to": [0, 1] } },
    { "id": "mask", "type": "veach:clamp@1", "inputs": { "x": "mask_raw" }, "params": { "min": 0, "max": 1 } },
    { "id": "height", "type": "veach:blend@1", "inputs": { "a": "land", "b": "peaks", "t": "mask" } }
  ],
  "outputs": { "height": "height" }
}
```

(Abridged; the file has every parameter.) Seven node types, each versioned: `veach:fbm@1`, `ridge@1`, `remap@1`,
`clamp@1`, `blend@1`, `add@1`, `mul@1`. Lengths need a unit class: a bare `"wavelength": 1000` is an error whose hint
says to write `{"planet_m": 1000}`. An unknown version (`veach:fbm@2`) names the versions this build has. The compiler
folds the body's radius into each fbm's frequency (6,000 km / 3,000 km = 2 on the unit sphere) and each remap into a
multiply and an add.

Op list: `[opcode, dst, a, b, c, n, payload[n]]` per op, 32-bit words, floats as their bits. The whole terrain is 78
words and uses 4 registers. The same words drive the CPU interpreter and are uploaded as the GPU interpreter's buffer.

Seeds follow ADR 0052: node seed = `hash(body_seed, hash_id(node_id))`, octave seed = `hash(node_seed, octave)` folded
to 32 bits, all computed once at compile time. The per-sample lattice hash is 32-bit integer only: the cell
coordinates times three odd constants, xor-ed with the octave seed, then the lowbias32 finalizer. Gradients are
Perlin's 12 cube edges (selects and one add, exact everywhere), with quintic fade and trilinear blend.

The sample grid is one cube-sphere face, 1024² samples (u, v in [-1, 1], projected to the unit sphere), for a 6,000 km
planet with seed 42.

## 1. Agreement

1024² samples, Release, Apple M5 Max (Metal). Heights span [0.0197, 1.0].

| Pair | Max abs difference | Mean abs difference |
|---|---|---|
| GPU interpreter vs CPU interpreter | 5.66e-7 | 2.6e-8 |
| Hand-written kernel vs CPU interpreter | 5.66e-7 | 2.5e-8 |
| GPU interpreter vs hand-written kernel | 1.19e-7 | 6.4e-9 |
| CPU batch vs CPU scalar (batch of 1) | 0 (bit-equal) | 0 |

At 4096² the largest difference is 6.56e-7. The unit test (256², Debug, Metal validation on) measures 4.47e-7.

| Backend | Result |
|---|---|
| Metal, M5 Max | passes, 5.7e-7 |
| DX12 on WARP | not run yet: `[sp06.AC3]` runs on Windows CI with the debug layer |
| CPU, arm64 Debug and Release, Apple Clang | the 128² output hash `0xFB7136EA481F1F8D`, committed in `[sp06.AC2]` |
| CPU, x86-64 Clang without FMA (the double route of section 3), run under Rosetta | the same hash |
| CPU, Windows x64 MSVC | not run yet: `[sp06.AC2]` checks the committed hash on Windows CI |

![CPU, GPU interpreter, hand-written, and the difference](sp-06/side_by_side.png)

*Left to right: the CPU interpreter, the GPU interpreter, the hand-written kernel (each normalized to the CPU's range,
2×2 box-filtered to 512²), and |GPU interpreter − CPU| with white at 1e-6, a tenth of the target. The differences sit
in the mountains, where the frequencies are highest and the ridge and the mask make the output most sensitive to its
inputs (the mask multiplies the continents' difference by 4).*

## 2. Speed

Release preset, M5 Max, one CPU thread. GPU times are the render graph's per-pass timestamps, median of 47 frames
after 3 warm-up frames, with the two kernels alternating which runs first.

| Evaluator | 1024² | Per 10⁶ samples | Throughput |
|---|---|---|---|
| CPU batch interpreter (256 per batch, NEON) | 52 ms | 49.8 ms | **20.1 M samples/s/core** (4.5 ns per noise octave) |
| CPU scalar interpreter (batch of 1) | 132 ms | 125.7 ms | 8.0 M samples/s/core |
| GPU interpreter | 0.296 ms | 0.282 ms | 3,550 M samples/s |
| GPU hand-written | 0.232 ms | 0.221 ms | 4,530 M samples/s |
| **GPU interpreter / hand-written** | | | **1.28×** |

At 4096² the GPU numbers are 4.62 ms and 3.60 ms (0.275 and 0.215 ms per 10⁶ samples), again 1.28×. Absolute GPU
times depend on the clock state the GPU happens to be in: an earlier 1024² run measured 0.48 and 0.38 ms, but the
ratio was 1.27×. Every run measured the ratio between 1.27× and 1.28×.

Where the interpreter's 28% goes, from two experiments (both reverted):

| Variant | Interpreter / hand-written |
|---|---|
| As committed: `float r[8]` indexed by op-list words | 1.28× |
| Hand-written kernel with its octave loops not unrolled (`[loop]`) | 1.28× (no change: unrolling is not what it gains) |
| Interpreter with registers in two `float4`s and select chains instead of an array | 1.38× (worse) |

So the cost is not the register array or the loop. What remains is what constant folding buys the hand-written kernel:
literal frequencies and seeds instead of uniform buffer loads inside the noise loop, and arithmetic folded on
literals. That would grow with smaller, cheaper nodes and shrink with heavier ones; here every op is a whole node
(fbm with all its octaves), so dispatch is paid per node, not per arithmetic operation.

**The CPU only reaches the target vectorized.** Before the noise was force-inlined, Clang did not vectorize the fbm
loop ("call instruction cannot be vectorized") and the batch interpreter ran at 8.7 M samples/s, no faster than the
scalar one. `[[gnu::always_inline]]` (MSVC: `__forceinline`) on the noise functions fixed it. The real interpreter
should not depend on the auto-vectorizer noticing: write the hot kernels with explicit 4- or 8-wide types, or check
the vectorizer remarks in CI. MSVC x64 is unmeasured; it auto-vectorizes less, and without FMA it takes the double
route (section 3), so expect it to be slower.

## 3. Fast math and the two fused operations

ADR 0043 keeps fast math on in shaders and turns contraction off on the CPU. For this kind of noise that alone gives
results that agree, but only just, and not at every frequency:

| Lattice fraction and fade | Terrain, max difference | One octave, frequency 10 | One octave, frequency 160 | One octave, frequency 2,560 |
|---|---|---|---|---|
| As written, CPU unfused, GPU fused by fast math | 9.7e-6 | 3.4e-6 | 5.5e-5 | 9.0e-4 |
| Fraction fused on every evaluator | 4.9e-6 | 2.4e-6 | 2.0e-6 | 1.3e-6 |
| Fraction and fade fused on every evaluator (committed) | **5.7e-7** | 3.3e-7 | | |

Two spots account for it:

- **The lattice fraction** `d = p·f − floor(p·f)`. Metal fuses it into `fma(p, f, −floor)`. The CPU rounds `p·f`
  first, losing up to half an ulp of `p·f`, which grows with the frequency. That is why the difference grew linearly
  with frequency, and why making the sample positions bit-identical on both sides (correctly rounded `sqrt` and
  divide, an explicit `fma` in the length; tried first, and kept) changed nothing.
- **The fade polynomial** `t·(6t − 15) + 10` falls from about 10 to about 1 as t nears 1. Fused or not, the result
  differs by ulp(10) ≈ 1e-6, which then multiplies the gradient difference.

The fix computes both as one correctly rounded `a·b + c` on every evaluator:

- CPU: `std::fma` where the target has hardware FMA (arm64, x86-64 with `-mfma`). Otherwise the product goes through
  double, where float × float is exact (24 + 24 bits) and so is the sum for these operands. Rounding that double to
  float gives fma's bits. `[sp06.AC2]` checks this on 600,000 operand triples, and the x86-64 build without FMA
  reproduces the arm64 hash. ADR 0043 allows explicit `fma` in canonical code, so this stays canonical.
- Metal: `fma` (always fused, correctly rounded).
- HLSL/DXIL: there is no float `fma`, and `mad` may or may not fuse, so the same route through double. This costs
  double-precision ops in the shader (per noise octave and axis: one fused op for the fraction, two for the fade), which WARP supports but which is
  optional (and slow) on D3D12 hardware. On Metal it costs nothing measurable.

Everything else may fuse or not and stays near one ulp (lerps, fbm accumulation, remap). Only spots where the result
cancels need this treatment. That is a property of each node's math, so each node needs its own check, which is what
the parity test is for.

## Do the decisions hold?

| ADR 0052 claim | Verdict |
|---|---|
| JSON IR with stable node ids and versioned node types | Holds. Stable ids carry the seeds, versions are checked, unit classes catch bare lengths, and the compiler to a flat op list is about 340 lines |
| CPU batch interpreter is canonical | Holds. Bit-equal across batch sizes, Debug/Release, and arm64 vs x86-64 without FMA (MSVC pending CI) |
| GPU interpreter is the visual evaluator; D3 only if it is over 2× hand-written | **Holds: 1.28×.** VCH-009 (run-time Slang) is not pulled forward |
| CPU ≥ 10 M samples/s/core for a realistic graph | Holds on arm64 when vectorized: 20 M for an 11-octave graph. MSVC unmeasured |
| Parity CPU/GPU within 1e-5 | Holds on Metal at 5.7e-7, **but only after the fused-operation rule**; without it 9.7e-6, and larger at higher frequencies |
| Every node ships a C++ batch kernel and a Slang function, checked by a parity test | Holds, and the test is necessary: it is what found the cancellation |

## Decisions for the dev lead

1. **Add a fused-operation rule to the node contract.** In node math where the result cancels, both evaluators
   compute `a·b + c` as one correctly rounded fused op: CPU `std::fma` or the exact double route, Metal `fma`, HLSL the
   double route. Without it, parity at high frequency fails. This belongs in the ADR that supersedes or amends 0052,
   together with the HLSL cost: either require double support on D3D12 hardware for field shaders, or accept a looser
   tolerance on D3D12 and keep `mad` there.
2. **Pick the lattice hash before `veach:fbm@1` is frozen** (ADR 0052 §10.1). The spike uses `veach::hash` only for
   seeds (per node and per octave, at compile time) and a 32-bit hash per lattice corner, because a 64-bit SplitMix
   per corner has no NEON multiply and would cost more than everything else per sample. Keep this split, or measure a
   `veach::hash` per corner variant first?
3. **Positions:** the spike feeds the unit direction in float32 and folds the radius into the frequency. Over a whole
   face that is fine. Near the surface, where frequencies reach 10⁴ to 10⁶ per radius, it needs the split form ADR
   0052 already specifies (integer cell plus float offset, computed in double per chunk). The fused fraction above
   does not replace it.

## Follow-ups

- Run `[sp06.AC2]` and `[sp06.AC3]` on Windows CI (MSVC hash, WARP agreement and the HLSL double route), and measure
  the CPU interpreter under MSVC.
- `engine/procgen`: explicit SIMD types for node kernels instead of relying on auto-vectorization; vec3 registers
  for domain warp; multi-output programs; `p_local` and `m` lengths; split positions.
- `procgen.*` error codes (the spike reuses `data.*`).
- A lint for canonical files that flags `a * b + c` patterns in node math that are meant to be fused but aren't.

## How it was measured

```sh
cmake --preset release && cmake --build --preset release --target veach-bench-sp06
build/release/tests/sp06/veach-bench-sp06 --resolution 1024 --frames 50 --repeats 5 --out docs/research/spikes/sp-06
build/release/tests/sp06/veach-bench-sp06 --resolution 4096 --frames 40 --repeats 2
build/debug/tests/veach-tests -tc="*sp06*"
```

CPU: best of 5 runs, one thread, `std::chrono::steady_clock`. GPU: `Device::last_frame_timings()` after `wait_idle()`
each frame, Metal validation off. The per-frequency rows of section 3 come from `--graph` with one-octave graphs,
measured with the Debug build (agreement does not depend on the build type: the Debug and Release hashes match).
Code: commit `b93ed21`.
