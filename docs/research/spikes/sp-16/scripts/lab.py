#!/usr/bin/env python3
"""Spike SP-16 (VCH-092): run the lab's vegetation field once, headless on Metal, and return what it logged.

  python3 docs/research/spikes/sp-16/scripts/lab.py --lab build/release/apps/lab/veach-lab \
      --env VEACH_SP16=vis VEACH_SP16_VIEW=mid --frames 4 --screenshot out.png

Used by shots.py and timings.py; on its own it prints the SP16_* lines.
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../.."))


def run(lab: str, env: dict[str, str], frames: int, screenshot: str | None = None) -> dict:
    """Runs the lab; returns {"exit": code, "timings": {...}, "stats": {...}, "cull": {...}, "log": text}."""
    run_env = {k: v for k, v in os.environ.items() if not k.startswith("VEACH_SP")}
    run_env.update(env)
    cmd = [lab, "--project", os.path.join(ROOT, "apps/lab"), "--rhi", "metal", "--headless", "--frames", str(frames)]
    if screenshot:
        cmd += ["--screenshot", os.path.abspath(screenshot)]
    proc = subprocess.run(cmd, env=run_env, capture_output=True, text=True, check=False)
    out = {"exit": proc.returncode, "log": proc.stdout + proc.stderr}
    for line in out["log"].splitlines():
        for tag, key in (("SP16_TIMINGS ", "timings"), ("SP16_STATS ", "stats"), ("SP16_CULL ", "cull")):
            if tag in line:
                out[key] = json.loads(line.split(tag, 1)[1])
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--env", nargs="*", default=[])
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--screenshot")
    args = parser.parse_args()
    env = dict(e.split("=", 1) for e in args.env)
    result = run(args.lab, env, args.frames, args.screenshot)
    for line in result["log"].splitlines():
        if "sp16" in line or "error" in line:
            print(line)
    return result["exit"]


if __name__ == "__main__":
    sys.exit(main())
