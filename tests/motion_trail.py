#!/usr/bin/env python3
"""The motion trail ("melting" edges, VCH-069): the final image after 30 frames of camera motion against a still
camera at the same pose. Whatever differs is history the renderer carried along. Bistro noon at 1280x720.

  python3 -I tests/motion_trail.py --lab <veach-lab> --imgdiff <veach> [--upscaler automatic|none]
      [--gi off|candela] [--max X] [--out DIR]

The camera holds still for 30 frames, then strafes 1.5 m over 30; the still run holds the final pose for all 60.
Prints the mean FLIP between the two final frames; fails above --max (when given).
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Mcp:
    def __init__(self, command: list[str]):
        self.proc = subprocess.Popen(command, cwd=ROOT / "apps" / "lab", stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
        self.stderr: list[str] = []
        threading.Thread(target=lambda: self.stderr.extend(self.proc.stderr), daemon=True).start()
        self.next_id = 0

    def call(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method,
                                          "params": params or {}}) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("the lab exited:\n" + "".join(self.stderr[-20:]))
        response = json.loads(line)
        if "error" in response:
            raise RuntimeError(str(response["error"]))
        return response["result"]

    def tool(self, name: str, **arguments):
        result = self.call("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(f"{name}: {result['content'][-1]['text']}")
        text = result["content"][0].get("text")
        return json.loads(text) if text and text[:1] in "{[" else result

    def close(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=120)


def run(lab: str, gi: str, upscaler: str, moving: bool, path: pathlib.Path) -> None:
    mcp = Mcp([lab, "--mcp", "--headless", "--rhi", "metal", "--script", "scripts/bistro_noon.luau"])
    mcp.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                            "clientInfo": {"name": "motion_trail", "version": "1"}})
    mcp.tool("step_frames", count=1)
    window = mcp.tool("get_component", entity="primary_window", component="Window")
    window.update(width=1280, height=720)
    mcp.tool("set_component", entity="primary_window", component="Window", value=window)
    mcp.tool("set_component", entity="camera", component="RealtimeLighting", value={"gi": gi})
    mcp.tool("set_component", entity="camera", component="Upscaling", value={"mode": upscaler})
    start = mcp.tool("get_component", entity="camera", component="Transform")
    for k in range(60):
        s = 1.0 if not moving else (0.0 if k < 30 else (k - 29) / 30)
        pose = dict(start)
        pose["translation"] = [start["translation"][0] - 1.5 * (1 - s), start["translation"][1],
                               start["translation"][2]]
        mcp.tool("set_component", entity="camera", component="Transform", value=pose)
        mcp.tool("step_frames", count=1)
    mcp.tool("screenshot", path=str(path))
    mcp.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lab", required=True)
    parser.add_argument("--imgdiff", required=True)
    parser.add_argument("--upscaler", default="automatic")
    parser.add_argument("--gi", default="off")
    parser.add_argument("--max", type=float)
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "build" / "motion_trail")
    args = parser.parse_args()
    lab = str(pathlib.Path(args.lab).resolve())
    if not (ROOT / "apps/lab/assets/bistro/BistroExterior.bin").exists():
        print("VEACH_GPU_UNAVAILABLE: skipped (Bistro's assets are not here)")
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    tag = f"{args.gi}_{args.upscaler}"
    moving, still = args.out / f"{tag}_moving.png", args.out / f"{tag}_still.png"
    try:
        run(lab, args.gi, args.upscaler, True, moving)
    except RuntimeError as error:
        if "rhi.backend_unavailable" in str(error):
            print("VEACH_GPU_UNAVAILABLE: skipped (no Metal ray tracing)")
            return 0
        raise
    run(lab, args.gi, args.upscaler, False, still)
    text = subprocess.run([args.imgdiff, "imgdiff", str(still), str(moving), "--max-mean", "0",
                           "--diff", str(args.out / f"{tag}_diff.png")], capture_output=True, text=True).stdout
    trail = float(re.search(r"mean FLIP ([\d.]+)", text)[1])
    print(f"motion trail, Bistro, gi {args.gi}, upscaler {args.upscaler}: FLIP {trail:.4f}"
          + (f" (limit {args.max})" if args.max is not None else ""))
    if args.max is not None and trail > args.max:
        print(f"motion_trail: {trail:.4f} exceeds {args.max}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
