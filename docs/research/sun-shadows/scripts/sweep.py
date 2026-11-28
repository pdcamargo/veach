#!/usr/bin/env python3
"""Sun shadows against the renderer before them (docs/research/sun-shadows/README.md): the GPU frame (the command
buffer's span) and GPU ms per pass for Bistro at noon and sunset, the SP-16 field (VEACH_SP16=engine) and the slice
near the ground, at 1080p and 1440p (the slice at its own 2560 x 1440).

  python3 docs/research/sun-shadows/scripts/sweep.py --new <tree> --old <tree> [--rounds 2] [--out timings.json]
  python3 docs/research/sun-shadows/scripts/sweep.py --table timings.json

<tree> is a source tree with a release build (build/release/apps/{lab,slice}). Configurations alternate between the
builds, `rounds` times; the tables take the median over rounds of each run's median (gpu-driven/scripts/timings.py).
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../gpu-driven/scripts"))
import timings  # noqa: E402

SIZES = ["1920x1080", "2560x1440"]


def configs(only: list[str]) -> list[dict]:
    out = []
    for size in SIZES:
        for time in ("noon", "sunset"):
            out.append({"name": f"bistro_{time}|{size}", "app": "lab", "script": f"scripts/bistro_{time}.luau",
                        "size": size, "env": {}})
        out.append({"name": f"field_ground|{size}", "app": "lab", "script": None, "size": size,
                    "env": {"VEACH_SP16": "engine", "VEACH_SP16_VIEW": "ground"}, "warmup": 60, "samples": 40})
    for scale in ("s0.01", "s1"):
        out.append({"name": f"slice_{scale}_landed|2560x1440", "app": "slice", "script": f"scripts/slice_{scale}.luau",
                    "size": "2560x1440", "env": {"VEACH_SLICE_AUTOPILOT": "1"}, "until": "autopilot: landed",
                    "samples": 60})
    return [c for c in out if not only or any(c["name"].startswith(o) for o in only)]


LANDED: dict[str, int] = {}


def run(tree: str, c: dict) -> dict:
    app = os.path.join(tree, "build/release/apps", c["app"], f"veach-{c['app']}")
    project = os.path.join(tree, "apps", c["app"])
    if c.get("until"):
        if c["script"] not in LANDED:
            LANDED[c["script"]] = timings.find_frame(app, project, c["script"], c["size"], c["env"], c["until"])
        return timings.measure_direct(app, project, c["script"], c.get("samples", 100), LANDED[c["script"]],
                                      c["env"])
    return timings.measure(app, project, c["script"], c["size"], c.get("warmup", 120), c.get("samples", 100),
                           c["env"])


def table(path: str) -> None:
    summary = json.load(open(path, encoding="utf-8"))
    print("| Scene | Size | Build | GPU frame | Shadow passes (sum) | Shade | Visibility | CPU ms/frame |")
    print("|---|---|---|---|---|---|---|---|")
    for name in sorted(summary):
        scene, size = name.split("|")
        for label in ("before", "shadows"):
            r = summary[name].get(label)
            if not r:
                continue
            shadow = sum(v for k, v in r["passes"].items() if k.startswith("shadow"))
            frame = f"{r['frame_ms']:.2f}" if r.get("frame_ms") is not None else "n/a"
            print(f"| {scene} | {size} | {label} | {frame} | {shadow:.2f} | {r['passes'].get('shade', 0):.2f} | "
                  f"{r['passes'].get('visibility', 0):.2f} | {r['wall_ms_per_frame']:.1f} |")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--new")
    parser.add_argument("--old")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../timings.json"))
    parser.add_argument("--table")
    args = parser.parse_args()
    if args.table:
        table(args.table)
        return 0
    results: dict[str, dict[str, list[dict]]] = {}

    def save() -> None:
        summary = {}
        for name, by_build in results.items():
            summary[name] = {}
            for label, runs in by_build.items():
                passes = sorted({p for run_ in runs for p in run_["passes"]})
                frames = [run_["frame_ms"] for run_ in runs if run_.get("frame_ms") is not None]
                summary[name][label] = {
                    "rounds": len(runs),
                    "total_ms": statistics.median(run_["total_ms"] for run_ in runs),
                    "frame_ms": statistics.median(frames) if frames else None,
                    "passes": {p: statistics.median(run_["passes"].get(p, 0.0) for run_ in runs) for p in passes},
                    "wall_ms_per_frame": statistics.median(run_["wall_ms_per_frame"] for run_ in runs),
                    "culling": next((run_["culling"] for run_ in reversed(runs) if run_.get("culling")), None),
                }
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=1, sort_keys=True)

    for r in range(args.rounds):
        for c in configs(args.only):
            for label, tree in (("before", args.old), ("shadows", args.new)):
                try:
                    result = run(tree, c)
                except RuntimeError as e:  # one failed run does not lose the sweep
                    print(f"round {r} {c['name']} {label}: failed: {e}", flush=True)
                    continue
                results.setdefault(c["name"], {}).setdefault(label, []).append(result)
                print(f"round {r} {c['name']} {label}: frame {result.get('frame_ms')} ms", flush=True)
                save()
    save()
    table(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
