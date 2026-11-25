# Profiling with Tracy

Veach is instrumented with [Tracy](https://github.com/wolfpld/tracy) v0.15.0 from day one
([ADR 0011](../adr/0011-profiling-tracy.md)). Performance claims in commits and PRs must cite numbers from a capture —
not estimates.

## Build

```sh
cmake --preset profile && cmake --build --preset profile    # RelWithDebInfo + VEACH_TRACY=ON
```

Without `VEACH_TRACY` the macros in `<veach/core/profile.hpp>` compile to nothing.
The client is built with `TRACY_ONLY_LOCALHOST` and `TRACY_NO_BROADCAST`, so it only accepts
connections from the same machine.

## Zones

| Zone | Where |
|---|---|
| `Engine::init`, `init step` (text: step name) | each init step |
| `Engine::start`, `Engine::shutdown` | startup script + Startup phase; teardown |
| `frame` (plus a frame mark per frame) | one main-loop iteration |
| `fixed step` | one 1/60 s simulation step |
| `phase` (text: phase name) | each phase run |
| `PollPlatform`, `SyncWindows` | platform systems in `Input` |
| `script.execute`, `script.system` (text: script / system name) | Luau top level; each script system run |
| `AssetServer::load` | asset loads |
| `Renderer::extract`, `extractor` (text: name), `Renderer::render` | extraction and render |

Add zones with `VEACH_ZONE("literal")`, or `VEACH_ZONE_TEXT("literal", text, size)` for a runtime label.

## Headless capture (agents, CI)

1. Build the command-line tools from the **same Tracy version** (the network protocol must match).
   After configuring the `profile` preset the source is in the CPM cache:

   ```sh
   TRACY=$(ls -d .cpm-cache/tracy/*/ | head -1)
   cmake -S "$TRACY/capture"   -B build/tracy-capture   -DCMAKE_BUILD_TYPE=Release && cmake --build build/tracy-capture
   cmake -S "$TRACY/csvexport" -B build/tracy-csvexport -DCMAKE_BUILD_TYPE=Release && cmake --build build/tracy-csvexport
   ```

   (`brew install tracy` also works, but only if it installs v0.15.0.)

2. Start the app with `--wait-for-profiler`, **then** the capture. The app blocks in init (up to 30 s)
   until a capture connects, so short `--frames` runs are recorded from the first zone. At exit it lingers
   500 ms so the capture can fetch zone names. Without that, every name in the CSV is `???`.
   (`TRACY_NO_EXIT=1` did not keep the client alive in our tests on macOS, and `tracy-capture` only connects
   to a client that is already listening. That is why the order matters.)

   ```sh
   build/profile/apps/lab/veach-lab --headless --frames 600 --wait-for-profiler &
   sleep 1
   build/tracy-capture/tracy-capture -o trace.tracy -f      # -f overwrites; exits when the app disconnects
   wait
   ```

   `tracy-capture` options (from its usage text): `-o output.tracy [-a address] [-p port] [-f] [-s seconds] [-m memlimit]`.
   It connects to `127.0.0.1:8086` by default.

   Verified on 2026-11-24 (Apple M5 Max, `profile` preset, 600 headless frames of the lab): `frame` mean
   ≈ 4.4 µs, `fixed step` ≈ 1.8 µs, `script.system` (the hello-world Greet system) ≈ 0.84 µs per run.
   `Engine::init` includes the time spent waiting for the capture to connect.

3. Export to CSV and read it as text:

   ```sh
   build/tracy-csvexport/tracy-csvexport trace.tracy > zones.csv           # one row per zone
   build/tracy-csvexport/tracy-csvexport -f phase trace.tracy              # only zones whose name contains "phase"
   build/tracy-csvexport/tracy-csvexport -u -f script.system trace.tracy   # every individual zone event
   build/tracy-csvexport/tracy-csvexport -F trace.tracy                    # frame statistics
   ```

## Reading the CSV

Default (aggregated) columns:

| Column | Meaning |
|---|---|
| `name`, `src_file`, `src_line` | zone and where it is declared |
| `total_ns` | total time in the zone over the capture |
| `total_perc` | share of the capture's total time |
| `counts` | number of zone events |
| `mean_ns`, `min_ns`, `max_ns`, `std_ns` | per-event statistics |

`-t[=P]` adds `percentile_ns` and `truncated_mean_ns`; `-e` reports self times (children excluded).
With `-u` the columns are `name, src_file, src_line, ns_since_start, exec_time_ns, thread, value`.

Example: per-frame cost of the simulation is the `mean_ns` of the `fixed step` row;
a script system's cost is the `script.system` row (use `-u` to see each run).
Compare like with like: same preset (`profile`), same `--frames`, same machine, and quote the
command you ran next to the numbers.
