#!/usr/bin/env python3
"""GPU ms per render-graph pass of a lab or slice scene (docs/research/gpu-driven/README.md).

  python3 docs/research/gpu-driven/scripts/timings.py --app build/release/apps/lab/veach-lab \
      --script scripts/bistro_noon.luau --size 1920x1080 [--warmup 120] [--samples 100] [--env NAME=value ...]
  python3 docs/research/gpu-driven/scripts/timings.py --app build/release/apps/slice/veach-slice \
      --project apps/slice --script scripts/slice_s1.luau --env VEACH_SLICE_AUTOPILOT=1 --until "autopilot: landed"

Runs the app headless on Metal as an MCP server, resizes the primary window, and steps frames back to back; the
renderer itself (VEACH_GPU_TIMINGS=samples,warmup) logs the median GPU ms of every pass over `samples` frames after
frame `warmup`. Timing inside the engine keeps the GPU fed: an Apple GPU left idle between frames clocks down and every
pass looks slower. With --until, a first run finds the frame at which the app logs that text (the slice's autopilot is
deterministic), and the second measures from there. Prints one JSON object with the passes, the total, the wall time
per frame and the last culling statistics.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../.."))
sys.path.insert(0, os.path.join(ROOT, "tests/mcp"))
from test_mcp_lab import McpClient  # noqa: E402


def start(app: str, project: str, script: str | None, size: str, env: dict[str, str]) -> McpClient:
    command = [app, "--mcp", "--headless", "--rhi", "metal", "--project", project]
    if script:
        command += ["--script", script]
    saved = dict(os.environ)
    os.environ.update(env)
    try:
        client = McpClient(command)
    finally:
        os.environ.clear()
        os.environ.update(saved)
    client.initialize()
    client.call_json("step_frames", count=1)  # startup systems run (scripts set their window size there)
    width, height = (int(v) for v in size.split("x"))
    window = next(e for e in client.call_json("query_entities", **{"with": ["Window"]})
                  if e["name"] == "primary_window")
    client.call_json("set_component", entity=window["id"], component="Window",
                     value={"width": width, "height": height})
    return client


def find_frame(app: str, project: str, script: str | None, size: str, env: dict[str, str], until: str,
               max_frames: int = 40000) -> int:
    client = start(app, project, script, size, env)
    frame = 1
    while not any(until in line for line in client.stderr):
        if frame >= max_frames:
            raise RuntimeError(f"'{until}' not logged in {max_frames} frames")
        frame = client.call_json("step_frames", count=25)["frame"]
    client.close()
    return frame


def measure(app: str, project: str, script: str | None, size: str, warmup: int, samples: int,
            env: dict[str, str], until: str | None = None) -> dict:
    first = warmup
    if until:
        first = find_frame(app, project, script, size, env, until)
    client = start(app, project, script, size, {**env, "VEACH_GPU_TIMINGS": f"{samples},{first}"})
    client.call_json("step_frames", count=max(1, first - 2))
    t0 = time.time()
    stepped = 0
    while not any("gpu_timings" in line for line in client.stderr):
        client.call_json("step_frames", count=50)
        stepped += 50
        if stepped > 4 * samples + 200:
            raise RuntimeError("no gpu_timings line:\n" + "".join(client.stderr[-20:]))
    wall_ms = (time.time() - t0) * 1000.0 / stepped
    culling = client.call_json("gpu_timings").get("culling")
    client.close()
    line = next(line for line in client.stderr if "gpu_timings" in line)
    report = json.loads(line.split("gpu_timings ", 1)[1])
    return {"size": size, "script": script, "env": env, "until": until, "from_frame": first,
            "total_ms": report["total_ms"], "passes": report["passes"], "wall_ms_per_frame": wall_ms,
            "culling": culling}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True)
    parser.add_argument("--project", default=os.path.join(ROOT, "apps/lab"))
    parser.add_argument("--script")
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--env", nargs="*", default=[])
    parser.add_argument("--until", help="measure from the frame at which a log line contains this text")
    args = parser.parse_args()
    env = dict(e.split("=", 1) for e in args.env)
    print(json.dumps(measure(args.app, args.project, args.script, args.size, args.warmup, args.samples, env,
                             args.until)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
