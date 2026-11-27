#!/usr/bin/env python3
"""How often the denoiser's history survives a still camera (VCH-067): per pixel and frame, the share that kept its
own history, borrowed a neighbor's (the same surface next to it) or reset, from the lab's counters at shutdown.

  python3 -I tools/bench/history_resets.py [--preset release] [--gi candela|off] scene ...

Three pipelines at 1280x720 output: no upscaler (no jitter, render 1280x720), MetalFX at render scale 1 (jitter, the
same render size) and MetalFX at 0.5 (the Performance tier). The first against the second isolates the jitter.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from frame_times import Mcp  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
PIPELINES = {
    "none": {"mode": "none", "render_scale": 1.0},
    "metalfx_1.0": {"mode": "metalfx", "render_scale": 1.0},
    "metalfx_0.5": {"mode": "metalfx", "render_scale": 0.5},
}


def measure(lab: pathlib.Path, scene: str, gi: str, upscaling: dict, frames: int) -> dict:
    mcp = Mcp([str(lab), "--mcp", "--headless", "--rhi", "metal", "--script", f"scripts/{scene}.luau"],
              ROOT / "apps" / "lab")
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                               "clientInfo": {"name": "history_resets", "version": "1"}})
    mcp.call("step_frames", count=1)
    window = mcp.call("get_component", entity="primary_window", component="Window")
    window.update(width=1280, height=720)
    mcp.call("set_component", entity="primary_window", component="Window", value=window)
    mcp.call("set_component", entity="camera", component="RenderQuality", value={"tier": "custom"})
    mcp.call("set_component", entity="camera", component="Upscaling", value=upscaling)
    mcp.call("set_component", entity="camera", component="RealtimeLighting", value={"gi": gi})
    mcp.call("step_frames", count=frames)
    mcp.close()
    line = next((l for l in mcp.stderr if "denoiser history:" in l), "")
    found = re.search(r"([\d.]+)% of pixels kept their own, ([\d.]+)% borrowed a neighbor's, ([\d.]+)% reset", line)
    if not found:
        raise RuntimeError("no denoiser history line in the lab's log:\n" + "".join(mcp.stderr[-10:]))
    return dict(zip(("kept", "borrowed", "reset"), (float(v) for v in found.groups())))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="+")
    parser.add_argument("--preset", default="release")
    parser.add_argument("--gi", default="candela")
    parser.add_argument("--frames", type=int, default=64)
    args = parser.parse_args()
    lab = ROOT / "build" / args.preset / "apps" / "lab" / "veach-lab"
    print("| Scene | Pipeline | Kept | Borrowed | Reset |")
    print("|---|---|---|---|---|")
    for scene in args.scenes:
        for name, upscaling in PIPELINES.items():
            r = measure(lab, scene, args.gi, upscaling, args.frames)
            print(f"| {scene} | {name} | {r['kept']:.2f}% | {r['borrowed']:.2f}% | {r['reset']:.2f}% |", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
