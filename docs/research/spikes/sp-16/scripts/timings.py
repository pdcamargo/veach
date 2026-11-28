#!/usr/bin/env python3
"""Spike SP-16 (VCH-092): GPU ms per pass of both renderers on the vegetation field, from render-graph timestamps.

  python3 docs/research/spikes/sp-16/scripts/timings.py --lab build/release/apps/lab/veach-lab [--rounds 2]

Each configuration is one headless lab run: VEACH_SP16_WARMUP frames for the GPU's clocks to settle, then the median
of VEACH_SP16_TIMINGS frames per pass (the lab logs it, sp16_spike.cpp). Rounds run every configuration in turn, and
the table takes the median over rounds. Writes timings.json next to this script and prints markdown tables.
"""
import argparse
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lab  # noqa: E402

PATHS = [
    ("vis", {"VEACH_SP16": "vis"}),
    ("forward", {"VEACH_SP16": "forward"}),
    ("prepass", {"VEACH_SP16": "prepass"}),
    ("forward_msaa", {"VEACH_SP16": "forward", "VEACH_SP16_MSAA": "4"}),
    ("prepass_msaa", {"VEACH_SP16": "prepass", "VEACH_SP16_MSAA": "4"}),
]
VIEWS = ["ground", "mid", "far"]
SIZES = ["1920x1080", "2560x1440"]


def measure(lab_path: str, env: dict[str, str], samples: int, warmup: int) -> dict:
    env = {**env, "VEACH_SP16_TIMINGS": str(samples), "VEACH_SP16_WARMUP": str(warmup)}
    result = lab.run(lab_path, env, warmup + 3 * samples + 10)  # some frames repeat a timing
    if result["exit"] != 0 or "timings" not in result:
        raise RuntimeError(f"{env}: exit {result['exit']}\n{result['log'][-2000:]}")
    return result["timings"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--samples", type=int, default=120)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--lights", default="16")
    parser.add_argument("--extra", nargs="*", default=[], help="extra VEACH_SP16_* settings, NAME=value")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "timings.json"))
    parser.add_argument("--views", nargs="*", default=VIEWS)
    parser.add_argument("--sizes", nargs="*", default=SIZES)
    parser.add_argument("--paths", nargs="*", default=[p for p, _ in PATHS])
    args = parser.parse_args()
    extra = dict(e.split("=", 1) for e in args.extra)
    runs: dict[str, list[dict]] = {}
    for r in range(args.rounds):
        for size in args.sizes:
            for view in args.views:
                for name, env in PATHS:
                    if name not in args.paths:
                        continue
                    key = f"{name}|{view}|{size}"
                    t = measure(args.lab, {**env, **extra, "VEACH_SP16_VIEW": view, "VEACH_SP16_SIZE": size,
                                           "VEACH_SP16_LIGHTS": args.lights}, args.samples, args.warmup)
                    runs.setdefault(key, []).append(t)
                    print(f"round {r} {key}: {t['total']:.3f} ms {t['passes']}", flush=True)
    table = {}
    for key, ts in runs.items():
        passes = sorted({p for t in ts for p in t["passes"]})
        table[key] = {
            "total": statistics.median(t["total"] for t in ts),
            "passes": {p: statistics.median(t["passes"].get(p, 0.0) for t in ts) for p in passes},
            "rounds": [t["total"] for t in ts],
        }
    with open(args.out, "w") as f:
        json.dump({"lights": args.lights, "extra": extra, "table": table}, f, indent=1)
    for size in args.sizes:
        print(f"\n### {size}\n")
        print("| path | " + " | ".join(args.views) + " |")
        print("|---|" + "---|" * len(args.views))
        for name, _ in PATHS:
            if name not in args.paths:
                continue
            cells = [f"{table[f'{name}|{v}|{size}']['total']:.2f}" for v in args.views]
            print(f"| {name} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
