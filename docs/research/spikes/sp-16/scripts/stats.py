#!/usr/bin/env python3
"""Spike SP-16 (VCH-092): what each view costs in geometry and fragments (VEACH_SP16_STATS), with and without HiZ.

  python3 docs/research/spikes/sp-16/scripts/stats.py --lab build/release/apps/lab/veach-lab

Per view at 1920x1080: visible instances and triangles after culling, the masked fragments rasterized per pixel (and
how many survive the alpha test), and the 2x2-quad lane efficiency of the visible surface.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lab  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    args = parser.parse_args()
    for view in ("ground", "mid", "far"):
        for hiz in ("1", "0"):
            r = lab.run(args.lab, {"VEACH_SP16": "vis", "VEACH_SP16_VIEW": view, "VEACH_SP16_STATS": "1",
                                   "VEACH_SP16_HIZ": hiz}, 14)
            assert r["exit"] == 0 and "cull" in r and "stats" in r, r["log"][-2000:]
            print(json.dumps({"view": view, "hiz": hiz == "1", **r["cull"], **r["stats"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
