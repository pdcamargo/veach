#!/usr/bin/env python3
"""GPU frame times of lab scenes, per render-graph pass (docs/specs/realtime-renderer-v0/spec.md, rt.AC9).

  python3 -I tools/bench/frame_times.py [--preset release] [--rhi metal] [--warmup 30] [--frames 120]
                                        [--size 2560x1440] [--breakdown] apps/lab/scripts/sponza.luau ...

Drives the lab over MCP (headless; the primary window is resized to --size after the script's own setup): steps
--warmup frames, then steps one frame at a time and reads `gpu_timings` after each. Prints the median and 95th
percentile of the frame's GPU time and the median of each pass. Run on a quiet machine: anything else using the
GPU skews the numbers. A first run cooks the scene's textures (cached in .veach/cache afterwards).

--breakdown times the shade pass again with one part left out at a time (the camera's RealtimeLighting: no AO
rays, no emissive light samples, no shadow rays, no alpha coverage tests), in the same session: what a part
costs is the full shade time minus the time without it.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import subprocess
import sys
import threading

ROOT = pathlib.Path(__file__).resolve().parents[2]


class Mcp:
    def __init__(self, command: list[str], cwd: pathlib.Path):
        self.proc = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.stderr: list[str] = []
        threading.Thread(target=lambda: self.stderr.extend(self.proc.stderr), daemon=True).start()
        self.next_id = 0

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method,
                                          "params": params or {}}) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("the lab exited:\n" + "".join(self.stderr[-20:]))
        response = json.loads(line)
        if "error" in response:
            raise RuntimeError(f"protocol error: {response['error']}")
        return response["result"]

    def call(self, tool: str, **arguments):
        result = self.request("tools/call", {"name": tool, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(f"{tool}: {result['content'][-1]['text']}")
        return json.loads(result["content"][0]["text"])

    def close(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=60)


def percentile(values: list[float], p: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(p * (len(ordered) - 1))))]


# (label, RealtimeLighting fields to change) for --breakdown; render::RealtimeLighting::debug_skip's bits.
VARIANTS = [
    ("full", {}),
    ("no AO rays", {"ao_rays": 0}),
    ("no light samples", {"light_candidates": 0}),
    ("no shadow rays", {"debug_skip": 1}),
    ("no alpha tests", {"debug_skip": 2}),
]


def measure(mcp: Mcp, scene: str, warmup: int, frames: int) -> tuple[list[float], dict[str, list[float]]]:
    mcp.call("step_frames", count=warmup)
    totals: list[float] = []
    passes: dict[str, list[float]] = {}
    for _ in range(frames):
        mcp.call("step_frames", count=1)
        timings = mcp.call("gpu_timings")
        if "passes_unavailable" in timings:
            raise RuntimeError(f"{scene}: no per-pass timings: {timings['passes_unavailable']}")
        totals.append(sum(p["gpu_ms"] for p in timings["passes"]))
        for p in timings["passes"]:
            passes.setdefault(p["name"], []).append(p["gpu_ms"])
    return totals, passes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="+", type=pathlib.Path)
    parser.add_argument("--preset", default="release")
    parser.add_argument("--rhi", default="metal")
    parser.add_argument("--warmup", type=int, default=30)
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--size", default="2560x1440", help="the primary window's size, set over MCP before measuring")
    parser.add_argument("--breakdown", action="store_true", help="also time the shade pass with each part left out")
    parser.add_argument("--lighting", action="append", default=[], metavar="FIELD=VALUE",
                        help="a RealtimeLighting field to set over MCP (repeatable), e.g. spatial_samples=2")
    parser.add_argument("--upscaler", default="automatic", choices=["automatic", "metalfx", "temporal", "none"],
                        help="the camera's Upscaling.mode, set over MCP (scenes may pin one for their goldens)")
    args = parser.parse_args()
    lab = ROOT / "build" / args.preset / "apps" / "lab" / "veach-lab"
    project = ROOT / "apps" / "lab"

    if args.breakdown:
        print("| Scene | " + " | ".join(f"shade, {label}" for label, _ in VARIANTS) + " |")
        print("|---|" + "---|" * len(VARIANTS))
    else:
        print("| Scene | Frame (median) | Frame (p95) | Passes (median ms) |")
        print("|---|---|---|---|")
    for scene in args.scenes:
        mcp = Mcp([str(lab), "--mcp", "--headless", "--rhi", args.rhi, "--script",
                   f"scripts/{scene.resolve().name}"], project)
        mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                   "clientInfo": {"name": "frame_times", "version": "1"}})
        mcp.call("step_frames", count=1)  # Startup systems (a script's own window size) run first
        width, height = (int(v) for v in args.size.split("x"))
        window = mcp.call("get_component", entity="primary_window", component="Window")
        window.update(width=width, height=height)
        mcp.call("set_component", entity="primary_window", component="Window", value=window)
        mcp.call("set_component", entity="camera", component="Upscaling", value={"mode": args.upscaler})
        if args.lighting:
            fields = {k: json.loads(v) for k, v in (item.split("=", 1) for item in args.lighting)}
            mcp.call("set_component", entity="camera", component="RealtimeLighting", value=fields)
        if args.breakdown:
            # A scene without RealtimeLighting renders with its defaults: add them so each variant starts from them.
            mcp.call("set_component", entity="camera", component="RealtimeLighting", value={})
            lighting = mcp.call("get_component", entity="camera", component="RealtimeLighting")
            cells = []
            for _, changes in VARIANTS:
                mcp.call("set_component", entity="camera", component="RealtimeLighting",
                         value={**lighting, **changes})
                _, passes = measure(mcp, scene.name, args.warmup, args.frames)
                cells.append(f"{statistics.median(passes['shade']):.2f}")
            mcp.close()
            print(f"| {scene.stem} | " + " | ".join(cells) + " |")
            continue
        totals, passes = measure(mcp, scene.name, args.warmup, args.frames)
        mcp.close()
        split = ", ".join(f"{name} {statistics.median(ms):.2f}" for name, ms in passes.items()
                          if statistics.median(ms) >= 0.05)
        print(f"| {scene.stem} | {statistics.median(totals):.2f} ms | {percentile(totals, 0.95):.2f} ms | {split} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
