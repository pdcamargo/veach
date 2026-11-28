#!/usr/bin/env python3
"""Ambient light and occlusion against the build before it (docs/research/ambient-light/README.md): GPU ms per pass
for Bistro, the SP-16 vegetation field through the engine (VEACH_SP16=engine) and the slice near the ground, at 1080p
and 1440p.

  python3 docs/research/ambient-light/scripts/sweep.py --new <tree> --old <tree> [--rounds 2] [--out timings.json]

<tree> is a source tree with a release build (build/release/apps/{lab,slice}). The measurement itself is the GPU-driven
core's (docs/research/gpu-driven/scripts/timings.py): the renderer times its passes while frames run back to back.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../../gpu-driven/scripts"))
import sweep as gpu_sweep  # noqa: E402

LABELS = ("before", "after")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--new", required=True)
    parser.add_argument("--old", required=True)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--out", default=os.path.join(HERE, "../timings.json"))
    args = parser.parse_args()
    results: dict[str, dict[str, list[dict]]] = {}

    def save() -> None:
        summary = {}
        for name, by_build in results.items():
            summary[name] = {}
            for label, runs in by_build.items():
                passes = sorted({p for run_ in runs for p in run_["passes"]})
                summary[name][label] = {
                    "rounds": len(runs),
                    "total_ms": statistics.median(run_["total_ms"] for run_ in runs),
                    "passes": {p: statistics.median(run_["passes"].get(p, 0.0) for run_ in runs) for p in passes},
                    "wall_ms_per_frame": statistics.median(run_["wall_ms_per_frame"] for run_ in runs),
                }
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=1, sort_keys=True)

    configs = [c for c in gpu_sweep.configs(args.only) if c["name"].split("|")[0] not in ("field_far",)]
    for r in range(args.rounds):
        for c in configs:
            for label, tree in zip(LABELS, (args.old, args.new)):
                try:
                    result = gpu_sweep.run(tree, c)
                except RuntimeError as e:
                    print(f"round {r} {c['name']} {label}: failed: {e}", flush=True)
                    continue
                results.setdefault(c["name"], {}).setdefault(label, []).append(result)
                print(f"round {r} {c['name']} {label}: {result['total_ms']:.3f} ms", flush=True)
                save()
    return 0


if __name__ == "__main__":
    sys.exit(main())
