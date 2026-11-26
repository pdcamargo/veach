#!/usr/bin/env python3
"""Candela steps 1 and 1b (ADR 0051): cost, temporal flicker and response to sudden changes, over MCP.

  python3 -I tools/bench/candela_step1.py [--preset release] [--rhi metal] [--quick] scene ...

Scenes are lab script names: cornell_box_realtime, sponza, bistro_noon. For each:

- Cost: frame_budget (ADR 0049) at 1440p output in the Performance tier, GI off (the ambient stopgap) and Candela:
  each Candela pass's median GPU time. The cache's statistics are in the lab's log at shutdown ("candela cache:").
- Flicker: a still camera at 640x360 without an upscaler; the mean FLIP between consecutive final frames (16 pairs),
  with Candela and with direct light only (candela.AC4: at most 0.005 with Candela).
- Response: a light switched off and a door opened (an occluder over the scene moved away). The indirect light is
  Candela's own (RealtimeLighting.view = "indirect": linear, exposed, before the denoiser), its mean over the
  image; the response is the number of frames until it has gone 90% of the way to where it settles 60 frames
  later (candela.AC5, AC8: within 10 frames). The denoiser's history adds its own lag to direct and indirect alike.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from compare_direct import mean_luminance  # noqa: E402
from frame_times import Mcp  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]

# A Cornell box scaled into a lid over the scene: its floor blocks the sun and much of the sky, its own lights shine
# inside it. Moving it away opens the "door".
LIDS = {
    "sponza": {"translation": [0, 27, 0], "scale": 20},
    "bistro_noon": {"translation": [-3, 40, -8], "scale": 30},
}


CANDELA: dict = {}  # --set: Candela fields applied in every session
FINAL = {"size": "640x360", "upscaling": None}  # --performance: the shipped view (1280x720, the tier's upscaler)


def session(lab: pathlib.Path, rhi: str, scene: str, size: str, gi: str, upscaling: dict | None) -> Mcp:
    mcp = Mcp([str(lab), "--mcp", "--headless", "--rhi", rhi, "--script", f"scripts/{scene}.luau"], ROOT / "apps" / "lab")
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                               "clientInfo": {"name": "candela_step1", "version": "1"}})
    mcp.call("step_frames", count=1)
    width, height = (int(v) for v in size.split("x"))
    window = mcp.call("get_component", entity="primary_window", component="Window")
    window.update(width=width, height=height)
    mcp.call("set_component", entity="primary_window", component="Window", value=window)
    mcp.call("set_component", entity="camera", component="Upscaling", value=upscaling or {"mode": "none"})
    mcp.call("set_component", entity="camera", component="RealtimeLighting", value={"gi": gi})
    if CANDELA:
        mcp.call("set_component", entity="camera", component="Candela", value=CANDELA)
    return mcp


def screenshot_luminance(mcp: Mcp, path: pathlib.Path) -> float:
    mcp.request("tools/call", {"name": "screenshot", "arguments": {"path": str(path)}})
    return mean_luminance(path)


def cost(lab: pathlib.Path, rhi: str, scene: str) -> dict:
    out = {}
    for gi in ("off", "candela"):
        mcp = session(lab, rhi, scene, "2560x1440", gi, {"mode": "automatic"})
        mcp.call("set_component", entity="camera", component="RenderQuality", value={"tier": "performance"})
        mcp.call("step_frames", count=30)
        report = mcp.call("frame_budget", tier="performance", frames=30)
        mcp.close()
        gi_row = next(s for s in report["subsystems"] if s["name"] == "gi")
        memory = next(k for k in report["memory"]["kinds"] if k["name"] == "gi")
        out[gi] = {"frame_ms": round(report["frame_gpu_ms"], 2), "gi_ms": round(gi_row["gpu_ms"], 2),
                   "passes": {p["name"]: round(p["gpu_ms"], 3) for p in gi_row["passes"]},
                   "gi_memory_mib": round(memory["mib"], 1)}
    return out


def flicker(lab: pathlib.Path, rhi: str, imgdiff: pathlib.Path, scene: str, work: pathlib.Path) -> dict:
    out = {}
    for gi in ("direct_only", "candela"):
        mcp = session(lab, rhi, scene, FINAL["size"], gi, FINAL["upscaling"])
        mcp.call("step_frames", count=96)
        shots = []
        for i in range(17):
            path = work / f"{scene}_{gi}_flicker_{i}.png"
            mcp.request("tools/call", {"name": "screenshot", "arguments": {"path": str(path)}})
            mcp.call("step_frames", count=1)
            shots.append(path)
        mcp.close()
        flips = []
        for a, b in zip(shots, shots[1:]):
            text = subprocess.run([str(imgdiff), "imgdiff", str(a), str(b), "--max-mean", "1", "--max-p999", "1",
                                   "--max-outliers", "1000000000"], capture_output=True, text=True).stdout
            flips.append(float(re.search(r"mean FLIP ([\d.]+)", text)[1]))
        out[gi] = round(statistics.mean(flips), 4)
    out["ratio"] = round(out["candela"] / max(out["direct_only"], 1e-6), 2)
    return out


def change_light_off(mcp: Mcp, scene: str) -> None:
    if scene == "bistro_noon":
        sky = mcp.call("get_component", entity="sky", component="Sky")
        sky["sun_illuminance"] = 0.0
        mcp.call("set_component", entity="sky", component="Sky", value=sky)
    elif scene == "sponza":
        mcp.call("set_component", entity="sun", component="DirectionalLight",
                 value={"color": [1, 0.95, 0.88], "illuminance": 0.0})
    else:
        lights = mcp.call("query_entities", **{"with": ["PointLight"]})
        for e in lights:
            mcp.call("set_component", entity=e["id"], component="PointLight", value={"intensity": 0.0})


def setup_door(mcp: Mcp, scene: str) -> None:
    if scene in LIDS:
        lid = LIDS[scene]
        s = lid["scale"]
        mcp.call("spawn_entity", name="door", components={
            "Transform": {"translation": lid["translation"], "rotation": {"x": 0, "y": 0, "z": 0, "w": 1},
                          "scale": [s, s, s]},
            "SceneInstance": {"path": "assets/cornell_box/cornell_box.gltf"}})


def open_door(mcp: Mcp, scene: str) -> None:
    if scene in LIDS:
        mcp.call("set_component", entity="door", component="Transform",
                 value={"translation": [0, 10000, 0], "rotation": {"x": 0, "y": 0, "z": 0, "w": 1},
                        "scale": [1, 1, 1]})
    else:  # the Cornell box: its tall box moved out of the room
        found = mcp.call("query_entities", **{"with": ["Transform", "MeshInstance"]})
        for e in found:
            if e["name"].endswith("tall box"):
                t = mcp.call("get_component", entity=e["id"], component="Transform")
                t["translation"] = [0, -100, 0]
                mcp.call("set_component", entity=e["id"], component="Transform", value=t)


def response(lab: pathlib.Path, rhi: str, scene: str, change: str, work: pathlib.Path, frames: int) -> dict:
    """Candela's own indirect light (RealtimeLighting.view = "indirect": linear, before the denoiser) after a change."""
    mcp = session(lab, rhi, scene, FINAL["size"], "candela", FINAL["upscaling"])
    mcp.call("set_component", entity="camera", component="RealtimeLighting", value={"gi": "candela", "view": "indirect"})
    if change == "door":
        setup_door(mcp, scene)
    mcp.call("step_frames", count=96)
    curve = [screenshot_luminance(mcp, work / f"{scene}_{change}_before.png")]
    if change == "light_off":
        change_light_off(mcp, scene)
    else:
        open_door(mcp, scene)
    for i in range(1, frames + 1):
        mcp.call("step_frames", count=1)
        curve.append(screenshot_luminance(mcp, work / f"{scene}_{change}_{i}.png"))
    mcp.call("step_frames", count=60)
    curve.append(screenshot_luminance(mcp, work / f"{scene}_{change}_settled.png"))
    mcp.close()
    before, settled = curve[0], curve[-1]
    span = before - settled
    reached = None
    for i, value in enumerate(curve[1:-1], start=1):
        if span != 0 and (before - value) / span >= 0.9:
            reached = i
            break
    return {"before": round(before, 5), "settled": round(settled, 5), "frames_to_90_percent": reached,
            "curve": [round(v, 5) for v in curve[:12]]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="+")
    parser.add_argument("--preset", default="release")
    parser.add_argument("--rhi", default="metal")
    parser.add_argument("--frames", type=int, default=20, help="frames recorded after each change")
    parser.add_argument("--skip", action="append", default=[], choices=["cost", "flicker", "response"])
    parser.add_argument("--performance", action="store_true",
                        help="flicker and response on the shipped view: 1280x720 output, the Performance tier's "
                             "upscaler (MetalFX at 0.5, jittered), instead of 640x360 without one")
    parser.add_argument("--set", action="append", default=[], metavar="FIELD=VALUE",
                        help="a Candela field for every session (repeatable), e.g. gather=\"surface_cache\"")
    args = parser.parse_args()
    CANDELA.update({k: json.loads(v) for k, v in (item.split("=", 1) for item in args.set)})
    if args.performance:
        FINAL.update(size="1280x720", upscaling={"mode": "automatic"})
    lab = ROOT / "build" / args.preset / "apps" / "lab" / "veach-lab"
    imgdiff = ROOT / "build" / args.preset / "tools" / "veach-cli" / "veach"
    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        for scene in args.scenes:
            r = {}
            if "cost" not in args.skip:
                r["cost"] = cost(lab, args.rhi, scene)
            if "flicker" not in args.skip:
                r["flicker"] = flicker(lab, args.rhi, imgdiff, scene, work)
            if "response" not in args.skip:
                r["light_off"] = response(lab, args.rhi, scene, "light_off", work, args.frames)
                r["door"] = response(lab, args.rhi, scene, "door", work, args.frames)
            results[scene] = r
            print(json.dumps({scene: r}, indent=1), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
