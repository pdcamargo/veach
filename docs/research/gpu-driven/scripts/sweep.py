#!/usr/bin/env python3
"""The GPU-driven renderer against the interim path (docs/research/gpu-driven/README.md): GPU ms per pass for Bistro,
the SP-16 vegetation field through the engine (VEACH_SP16=engine) and the slice near the ground, at 1080p and 1440p.

  python3 docs/research/gpu-driven/scripts/sweep.py --new <tree> --old <tree> [--rounds 3] [--out timings.json]

<tree> is a source tree with a release build (build/release/apps/{lab,slice}). Every configuration runs on both
builds in turn, `rounds` times; the tables take the median over rounds of each run's median (timings.py).
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import timings  # noqa: E402

SIZES = ["1920x1080", "2560x1440"]


def configs(only: list[str]) -> list[dict]:
    out = []
    for size in SIZES:
        out.append({"name": f"bistro|{size}", "app": "lab", "script": "scripts/bistro_noon.luau", "size": size,
                    "env": {}})
        for view in ("ground", "mid", "far"):
            out.append({"name": f"field_{view}|{size}", "app": "lab", "script": None, "size": size,
                        "env": {"VEACH_SP16": "engine", "VEACH_SP16_VIEW": view}, "warmup": 60, "samples": 40})
        for scale in ("s0.01", "s1"):
            out.append({"name": f"slice_{scale}_landed|{size}", "app": "slice", "script": f"scripts/slice_{scale}.luau",
                        "size": size, "env": {"VEACH_SLICE_AUTOPILOT": "1"}, "until": "autopilot: landed",
                        "samples": 60})
    return [c for c in out if not only or any(c["name"].startswith(o) for o in only)]


def run(tree: str, c: dict) -> dict:
    app = os.path.join(tree, "build/release/apps", c["app"], f"veach-{c['app']}")
    project = os.path.join(tree, "apps", c["app"])
    return timings.measure(app, project, c["script"], c["size"], c.get("warmup", 120), c.get("samples", 100),
                           c["env"], c.get("until"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--new", required=True)
    parser.add_argument("--old", required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../timings.json"))
    args = parser.parse_args()
    results: dict[str, dict[str, list[dict]]] = {}
    for r in range(args.rounds):
        for c in configs(args.only):
            for label, tree in (("interim", args.old), ("gpu_driven", args.new)):
                result = run(tree, c)
                results.setdefault(c["name"], {}).setdefault(label, []).append(result)
                print(f"round {r} {c['name']} {label}: {result['total_ms']:.3f} ms", flush=True)
    summary = {}
    for name, by_build in results.items():
        summary[name] = {}
        for label, runs in by_build.items():
            passes = sorted({p for run_ in runs for p in run_["passes"]})
            summary[name][label] = {
                "total_ms": statistics.median(run_["total_ms"] for run_ in runs),
                "passes": {p: statistics.median(run_["passes"].get(p, 0.0) for run_ in runs) for p in passes},
                "wall_ms_per_frame": statistics.median(run_["wall_ms_per_frame"] for run_ in runs),
                "culling": runs[-1].get("culling"),
            }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1, sort_keys=True)
    print(json.dumps({k: {b: round(v["total_ms"], 3) for b, v in d.items()} for k, d in summary.items()}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
