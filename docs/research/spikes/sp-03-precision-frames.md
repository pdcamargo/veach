# SP-03: precision ladder, propagation cost and Luau `Position` (VCH-079)

Validates [ADR 0045](../../adr/0045-large-world-positions-and-reference-frames.md) (doubles relative to frames,
camera-relative float32 on the GPU, computed in double at extraction) and
[ADR 0046](../../adr/0046-64-bit-values-across-luau-json-mcp.md) (a `Position` userdata in Luau). Research:
[01-coordinates](../01-coordinates.md), spikes S1.1 and S1.3. All numbers are from an Apple M5 Max, 2026-11-27.

## What was built (throwaway)

No engine type changed. The spike code lives in the lab and in two bench executables:

- `apps/lab/src/sp03_spike.{hpp,cpp}`: a `Sp03LargePosition { double x, y, z }` component, and an extractor
  registered after the engine's (`sp03_camera_relative`). An entity with `Sp03LargePosition` sits at that double
  offset plus its float32 `GlobalTransform`, which stays small. When the primary camera has one, the extractor rewrites
  the frame: the view is the camera's rotation at the origin (`view.position = 0`), each `MeshDraw.world` and each
  light position becomes `vec3(dvec3(local) + (anchor − camera))`, with the subtraction in double. The GPU still gets
  float32. It also spawns an `Sp03Ladder` case from `VEACH_SP03_DISTANCE`, `VEACH_SP03_MODE` (`float`, `double`) and
  `VEACH_SP03_PHOTO`.
- `apps/lab/scripts/sp03_ladder.luau`: the Cornell box and a camera 3.9 m from it (the view in
  `cornell_box_realtime.luau`), moved `distance` meters from the origin along (0.6, 0.48, 0.64), so all three
  coordinates are large. Mode `float` places both with float32 `Transform`s, which is how it works today. Mode `double`
  leaves the `Transform`s at the origin and puts the offsets in `Sp03LargePosition`. The camera's offset is computed
  in Luau doubles, so the extractor really subtracts two large numbers.
- `tests/bench/sp03_propagation_bench.cpp` (`veach-bench-sp03-propagation`): float vs double propagation and
  extraction over 100k entities.
- `tests/bench/sp03_position_bench.cpp` (`veach-bench-sp03-position`): a tagged `Position` userdata (three doubles,
  `__add`, `__sub`, `__mul`, `__index` by atom, `__namecall` for `:relative(origin)` and `:length()`) against native
  `vector`. The VM is set up like the engine's: interpreter only, `-O1`, sandboxed.

Reproduce (from `apps/lab`, debug build):

```sh
VEACH_SP03_DISTANCE=1.496e11 VEACH_SP03_MODE=double ../../build/debug/apps/lab/veach-lab --rhi metal --headless \
  --frames 3 --script scripts/sp03_ladder.luau --screenshot out.png      # VEACH_SP03_PHOTO=1 and --frames 4: photo mode
../../build/debug/tools/veach-cli/veach imgdiff <origin>.png out.png
build/release/tests/veach-bench-sp03-propagation 200
build/release/tests/veach-bench-sp03-position 1000000 5
```

## 1. Precision ladder

256×256, real-time renderer (3 frames) unless marked photo (path tracer, 4 frames × 16 spp). "Mean FLIP" is
`veach imgdiff` against `float_0.png` (the box at the origin, rendered the current way; identical to the committed
`cornell_box_realtime.png` golden, mean 0.00000) or against `float_photo_0.png`. The default pass limits are mean
0.01, 99.9th percentile 0.25, no pixel above 0.5. float32 ULP is for the largest coordinate (0.64 × distance). A pixel
at the box is about 11 mm wide.

| Distance | Current (float32) | Mean FLIP | What it looks like | Camera-relative (double) | Mean FLIP | vs double at 0 |
|---|---|---|---|---|---|---|
| 0 | [float_0](sp-03/float_0.png) | 0.00000 | reference | [double_0](sp-03/double_0.png) | 0.00013 PASS | — |
| 1 km (ULP 0.06 mm) | [float_1km](sp-03/float_1km.png) | 0.0109 **FAIL** (35 px > 0.5) | looks right; checker and stripe edges shift by a pixel | [double_1km](sp-03/double_1km.png) | 0.00013 PASS | 0.00000 |
| 10 km (0.5 mm) | [float_10km](sp-03/float_10km.png) | 0.039 FAIL | checker breaks into blocks, stripes jagged | — | 0.00013 PASS | — |
| 64 km (3.9 mm) | [float_64km](sp-03/float_64km.png) | 0.127 FAIL | banded walls, stripes become a checkerboard | — | 0.00013 PASS | — |
| 1,000 km (6 cm) | [float_1000km](sp-03/float_1000km.png) | 0.449 FAIL | walls black and see-through, textures gone, boxes misplaced | [double_1000km](sp-03/double_1000km.png) | 0.00013 PASS | 0.00000 |
| 6,371 km (25 cm) | [float_6371km](sp-03/float_6371km.png) | 0.530 FAIL | one floor wedge, the light and a flat box | [double_6371km](sp-03/double_6371km.png) | 0.00013 PASS | — |
| 1 AU (8 km) | [float_1AU](sp-03/float_1AU.png) | 0.523 FAIL | black: the geometry collapsed | [double_1AU](sp-03/double_1AU.png) | 0.00014 PASS | 0.00001 |
| 30 AU (262 km) | [float_30AU](sp-03/float_30AU.png) | 0.523 FAIL | black | [double_30AU](sp-03/double_30AU.png) | 0.00055 PASS | 0.00044 |

Photo mode (TLAS instances come from the `MeshDraw` world matrices):

| Distance | Current (float32) | Mean FLIP | Camera-relative (double) | Mean FLIP | vs double photo at 0 |
|---|---|---|---|---|---|
| 0 | (reference) | — | [double_photo_0](sp-03/double_photo_0.png) | 0.00083 PASS | — |
| 1 AU | [float_photo_1AU](sp-03/float_photo_1AU.png) | 0.874 FAIL (black) | [double_photo_1AU](sp-03/double_photo_1AU.png) | 0.00102 PASS | 0.00084 |
| 30 AU | [float_photo_30AU](sp-03/float_photo_30AU.png) | 0.874 FAIL: every sample NaN (`render.nonfinite_samples`, exit 1) | [double_photo_30AU](sp-03/double_photo_30AU.png) | 0.00390 PASS | 0.00383 |

Photo mode's own noise floor: two runs of `cornell_box.luau` differ by mean FLIP 0.0004, so the 1 AU result sits at
that noise.

What I saw, looking at every image:

- **Camera-relative is indistinguishable from the origin at every distance.** Between 1 km and 1 AU the double images
  are bit-identical to `double_0` or within 0.00001. At 30 AU there are sub-pixel edge changes (mean 0.00044, worst
  pixel 0.30), because the double ULP of the barycenter frame there is 0.5 mm, as ADR 0045's table predicts, and the
  3.9 m eye offset is rounded to it. In the real design the box and camera would share a local frame and that goes
  away.
- `double_0` vs `float_0` (0.00013, worst 0.43) is the same scene computed with different float roundings (camera at
  the origin vs at 3.9 m). It shows on a few triangle-edge pixels only; by eye the two images are the same.
- **The current renderer breaks earlier than the research estimated** (wobble "beyond ~10-20 km"). It fails the golden
  threshold at 1 km and is visibly wrong at 10 km. Vertex quantization isn't the cause: at 64 km it is 0.35 px. The
  real-time shade pass rebuilds the camera ray from `inverse_view_projection` and intersects the triangle plane in
  world space (`realtime_shade.slang`). With a large translation in the matrix, that ray loses its precision long
  before the vertices do. Camera-relative views fix this as a side effect, since the matrices hold no large numbers.

## 2. Propagation and extraction cost

Release preset, single thread, 100,000 entities, mean of 200 passes, run twice (ranges shown). Variants: today's
float32 `mat4`, a full `dmat4`, and ADR 0045's `FrameTransform` (double translation, float32 3×3 basis; local
`Transform` with a `dvec3` translation). "ECS" is a Flecs cascade query over `ChildOf`, like the engine's system;
"flat" is the same math over arrays sorted parents first.

| Hierarchy | Path | float `mat4` | `dmat4` | `FrameTransform` |
|---|---|---|---|---|
| 1,000 roots × 9 × 10 (depth 3) | ECS | 1.19 ms | 2.03-2.05 ms (×1.7) | 1.29-1.33 ms (×1.07-1.12) |
| 10,000 chains × 10 (depth 10) | ECS | 21.0-23.2 ms | 22.5-23.1 ms | 16.3-17.3 ms |
| either | flat | 0.56-0.57 ms | 1.31 ms (×2.3) | 0.81-0.87 ms (×1.45-1.5) |

Extraction, 100k camera-relative float32 draw matrices: copying a float `mat4` (today) takes 0.093 ms,
`dmat4 − camera → mat4` 0.178 ms, and `FrameTransform − camera → mat4` 0.199 ms.

- **ADR 0045's choice costs +0.1 ms per 100k entities in the ECS, and +0.1 ms in extraction.** A full `dmat4` would
  cost +0.85 ms. All three variants agree on the last entity's position to 1e-6 m.
- **The hierarchy's shape matters about 20× more than float vs double.** `ChildOf` fragments tables: every parent's
  children get a table of their own, so 10k chains become about 90k one-entity tables, and float propagation takes
  21 ms. That is a cost today, unrelated to ADR 0045, and it decides Q7's entity budgets.
- **Engine bug found:** `veach::propagate_transforms(world)` builds a cascade query on every call
  (`engine/ecs/src/core_module.cpp`). Cached queries are entity-backed, so the destructor doesn't free them. Each call
  costs 8.5 ms (wide) to 120 ms (chains) instead of 1.2-21 ms. A few hundred leaked queries crashed `ecs_fini` in
  release (SIGSEGV in `flecs_query_cache_on_rematch_event`). The engine's systems are not affected; tests and tools
  that call the function are.

## 3. Luau `Position` cost

`-O1` interpreter (the engine's settings), 1,000,000 operations per case, best of 5, minus the empty loop
(1.2 ns/iteration). `-O2` gives the same `Position` numbers.

| Operation | `vector` (float32) | `Position` (3 doubles) | Allocates a userdata |
|---|---|---|---|
| `a + d` (`Position + vector → Position`) | 0.7-1.4 ns | 15.2-15.3 ns | yes |
| `a − d` (`Position − vector → Position`) | — | 15.7-15.8 ns | yes |
| `a − b` (`Position − Position → vector`) | 0.3 ns | 7.4-7.5 ns | no |
| `a * s` (not in ADR 0046) | 0.8-1.3 ns | 15.8-16.0 ns | yes |
| length (`vector.magnitude` / `p:length()`) | 1.2-1.8 ns | 6.6-7.3 ns | no |
| to float32 relative to an origin (`v − o` / `p:relative(o)`) | 0.3 ns | 7.5-7.9 ns | no |
| field read `p.x` | — | 7.0-7.1 ns | no |
| `pos = pos + vel * dt` | 1.0-1.1 ns | 15.4-15.7 ns | yes |

- **Per 0.5 ms on the M5 Max:** about 30,000 allocating `Position` operations, or about 65,000 that return a `vector`
  or a number (native `vector`: about 250,000-500,000). For M1-class, assuming roughly half the M5 Max's
  single-thread speed (an assumption, not measured): about 15,000 / 30,000.
- **For context, today's script systems:** `veach-bench-script` costs 90 ns per entity just to invoke a Luau system,
  and 179 ns for a read-write system (`e:set` with a `vector`). One `Position` operation per entity adds 8-17% to
  that. The per-entity call, not position math, sets the budget.
- **Not measured:** marshalling `Transform.translation` as a `Position` would allocate one userdata per component read
  (about one `pos_add`'s cost, 15 ns) and adds GC pressure in systems that read many transforms.

## Do the decisions hold?

- **ADR 0045 holds.** Camera-relative extraction computed in double is imgdiff-clean from 1 km to 30 AU, in real time
  and in photo mode, and `FrameTransform` propagation costs +0.1 ms per 100k entities. The reopen condition ("double
  propagation measurably expensive") is not met. Refinements for the implementation:
  - **One render origin per frame, or per view?** `MeshDraw` has one world matrix for every view, so the spike made
    everything relative to the primary camera. The ADR says "for each view". The cheapest engine shape is one render
    origin per `RenderWorld` (the primary camera), with each other view's view matrix carrying its float32 offset from
    that origin. That is exact for views near the primary (shadow and light views, a second player in the same place)
    and wrong only for a far second camera, which would then need per-view draw matrices.
  - `RenderLight` has no entity id. The spike had to match lights by extraction order; the real change does the
    subtraction in the scene extractor itself.
  - Shaders that rebuild positions from `inverse_view_projection` (real-time shade, path tracer, atmosphere) are
    fine once the view sits at the origin. The atmosphere's `view.position.y` altitude must come from the body
    context, as the ADR already says. The spike used no sky.
- **ADR 0046 holds.** `Position` is 10-20× slower than `vector` per operation but small next to the per-entity cost of
  a script system. Its reopen condition ("`Position` too slow for gameplay scripts") is not met. Keep "no scaling of a
  `Position`": `*` costs the same as `+` and has no meaning for a frame-relative point.

## Follow-ups (for the backlog; not filed by this spike)

- Fix `propagate_transforms()`: build its query once per world (or use an uncached query) and add a release-build test
  that calls it repeatedly.
- `ChildOf` fragmentation: measure deep and wide hierarchies at Q7's counts, and whether Flecs v4.2 offers a
  non-fragmenting parent relationship for propagation.
- When ADR 0045 is implemented: decide the render-origin shape above, add entity ids (or anchors) to lights, and use
  this ladder (1 km, 1 AU, 30 AU, against the origin image) as golden tests.
- Photo mode is not reproducible from one run to the next (mean FLIP 0.0004 between two runs of the same golden
  script). Worth knowing before goldens get tighter.
