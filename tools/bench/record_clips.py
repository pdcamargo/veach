#!/usr/bin/env python3
"""Short MP4s of the final image, for judging flicker and smear by eye (docs/specs/candela/spec.md, VCH-069).

  python3 -I tools/bench/record_clips.py [--preset release] [--out build/videos] [--size 1280x720]
      [--variant name=JSON ...] scene ...

For each scene and variant (a name and the camera's Candela and RealtimeLighting fields, as JSON; default: GI off),
three clips at 30 fps after 96 frames of warm-up, in the Performance tier with its upscaler: `static` (5 s, nothing
moves), `light_off` (1 s, then the scene's light switches off, 4 s more) and `pan` (the camera turns 15 degrees over
5 s). Frames come over MCP; tools/bench/png_to_mp4.swift encodes them with AVFoundation. Writes
<out>/<scene>_<variant>_<clip>.mp4.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from frame_times import Mcp  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
FPS = 30


def quat_mul(a: dict, b: dict) -> dict:
    return {"w": a["w"] * b["w"] - a["x"] * b["x"] - a["y"] * b["y"] - a["z"] * b["z"],
            "x": a["w"] * b["x"] + a["x"] * b["w"] + a["y"] * b["z"] - a["z"] * b["y"],
            "y": a["w"] * b["y"] - a["x"] * b["z"] + a["y"] * b["w"] + a["z"] * b["x"],
            "z": a["w"] * b["z"] + a["x"] * b["y"] - a["y"] * b["x"] + a["z"] * b["w"]}


def light_off(mcp: Mcp, scene: str) -> None:
    if scene.startswith("bistro"):
        sky = mcp.call("get_component", entity="sky", component="Sky")
        sky["sun_illuminance"] = 0.0
        mcp.call("set_component", entity="sky", component="Sky", value=sky)
    elif scene == "sponza":
        light = mcp.call("get_component", entity="sun", component="DirectionalLight")
        light["illuminance"] = 0.0
        mcp.call("set_component", entity="sun", component="DirectionalLight", value=light)
    else:
        for e in mcp.call("query_entities", **{"with": ["PointLight"]}):
            light = mcp.call("get_component", entity=e["id"], component="PointLight")
            light["intensity"] = 0.0
            mcp.call("set_component", entity=e["id"], component="PointLight", value=light)


def record(lab: pathlib.Path, scene: str, size: str, settings: dict, clip: str, frames_dir: pathlib.Path) -> None:
    mcp = Mcp([str(lab), "--mcp", "--headless", "--rhi", "metal", "--script", f"scripts/{scene}.luau"],
              ROOT / "apps" / "lab")
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                               "clientInfo": {"name": "record_clips", "version": "1"}})
    mcp.call("step_frames", count=1)
    width, height = (int(v) for v in size.split("x"))
    window = mcp.call("get_component", entity="primary_window", component="Window")
    window.update(width=width, height=height)
    mcp.call("set_component", entity="primary_window", component="Window", value=window)
    mcp.call("set_component", entity="camera", component="Upscaling", value={"mode": "automatic"})
    for component, value in settings.items():
        mcp.call("set_component", entity="camera", component=component, value=value)
    mcp.call("step_frames", count=96)
    start = mcp.call("get_component", entity="camera", component="Transform")
    count = 5 * FPS
    for i in range(count):
        if clip == "light_off" and i == FPS:
            light_off(mcp, scene)
        if clip == "pan":
            yaw = math.radians(15.0) * i / (count - 1)
            pose = dict(start)
            pose["rotation"] = quat_mul({"x": 0.0, "y": math.sin(yaw / 2), "z": 0.0, "w": math.cos(yaw / 2)},
                                        start["rotation"])
            mcp.call("set_component", entity="camera", component="Transform", value=pose)
        mcp.call("step_frames", count=1)
        mcp.request("tools/call", {"name": "screenshot",
                                   "arguments": {"path": str(frames_dir / f"frame_{i:04d}.png")}})
    mcp.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="+")
    parser.add_argument("--preset", default="release")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "build" / "videos")
    parser.add_argument("--size", default="1280x720")
    parser.add_argument("--clips", default="static,light_off,pan")
    parser.add_argument("--variant", action="append", default=[], metavar="NAME=JSON",
                        help='e.g. option0=\'{"RealtimeLighting": {"gi": "candela"}, "Candela": {"adaptive_cache": true}}\'')
    args = parser.parse_args()
    lab = ROOT / "build" / args.preset / "apps" / "lab" / "veach-lab"
    variants = [(v.split("=", 1)[0], json.loads(v.split("=", 1)[1])) for v in args.variant] or \
        [("gi_off", {"RealtimeLighting": {"gi": "off"}})]
    args.out.mkdir(parents=True, exist_ok=True)
    for scene in args.scenes:
        for name, settings in variants:
            for clip in args.clips.split(","):
                frames_dir = args.out / f"{scene}_{name}_{clip}_frames"
                shutil.rmtree(frames_dir, ignore_errors=True)
                frames_dir.mkdir()
                record(lab, scene, args.size, settings, clip, frames_dir)
                movie = args.out / f"{scene}_{name}_{clip}.mp4"
                subprocess.run(["xcrun", "swift", str(ROOT / "tools" / "bench" / "png_to_mp4.swift"), str(frames_dir),
                                str(movie), str(FPS)], check=True)
                shutil.rmtree(frames_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
