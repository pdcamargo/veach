#!/usr/bin/env python3
"""GPU ms per render-graph pass of a lab or slice scene, through MCP (docs/specs/gpu-driven-renderer/spec.md).

  python3 docs/research/gpu-driven/scripts/timings.py --app build/release/apps/lab/veach-lab \
      --script scripts/bistro_noon.luau --size 1920x1080 [--warmup 120] [--samples 60] [--env NAME=value ...]

Starts the app headless on Metal as an MCP server, resizes the primary window, steps `warmup` frames, then `samples`
times steps `stride` frames back to back and reads `gpu_timings` (the last of them). Prints one JSON object: the median ms of every pass, the
median total, and the last culling statistics. The interim renderer's build answers the same tool, so one script
measures both.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../.."))
sys.path.insert(0, os.path.join(ROOT, "tests/mcp"))
from test_mcp_lab import McpClient  # noqa: E402


def measure(app: str, project: str, script: str | None, size: str, warmup: int, samples: int, stride: int,
            env: dict[str, str]) -> dict:
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
    client.call_json("step_frames", count=warmup)
    passes: dict[str, list[float]] = {}
    totals: list[float] = []
    culling = None
    for _ in range(samples):
        # Several frames back to back before each reading: a GPU left idle between single steps clocks down.
        client.call_json("step_frames", count=stride)
        t = client.call_json("gpu_timings")
        frame_total = 0.0
        for p in t["passes"]:
            passes.setdefault(p["name"], []).append(p["gpu_ms"])
            frame_total += p["gpu_ms"]
        totals.append(frame_total)
        culling = t.get("culling", culling)
    client.close()
    return {"size": size, "script": script, "total_ms": statistics.median(totals),
            "passes": {name: statistics.median(v) for name, v in passes.items()}, "culling": culling}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True)
    parser.add_argument("--project", default=os.path.join(ROOT, "apps/lab"))
    parser.add_argument("--script")
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--samples", type=int, default=60)
    parser.add_argument("--stride", type=int, default=8, help="frames stepped before each reading")
    parser.add_argument("--env", nargs="*", default=[])
    args = parser.parse_args()
    env = dict(e.split("=", 1) for e in args.env)
    print(json.dumps(measure(args.app, args.project, args.script, args.size, args.warmup, args.samples, args.stride, env)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
