#!/usr/bin/env python3
"""Markdown tables from sweep.py's timings.json (docs/research/gpu-driven/README.md).

  python3 docs/research/gpu-driven/scripts/tables.py [docs/research/gpu-driven/timings.json]
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GROUPS = {
    "culling": lambda p: p.startswith("cull"),
    "visibility": lambda p: p.startswith("visibility"),
    "HiZ": lambda p: p.startswith("hiz"),
    "classify": lambda p: p.startswith("classify"),
    "shade": lambda p: p == "shade",
}


def group(passes: dict) -> dict:
    out = {g: sum(v for p, v in passes.items() if f(p)) for g, f in GROUPS.items()}
    out["other"] = sum(passes.values()) - sum(out.values())
    return out


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "../timings.json")
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    header = "| Scene | Size | Path | Total | Culling | Visibility | HiZ | Classify | Shade | Other | CPU ms/frame |"
    print(header)
    print("|" + "---|" * (header.count("|") - 1))
    for name in sorted(data, key=lambda n: (n.split("|")[0], n.split("|")[1])):
        scene, size = name.split("|")
        for label in ("interim", "gpu_driven"):
            r = data[name].get(label)
            if r is None:
                continue
            g = group(r["passes"])
            print(f"| {scene} | {size} | {label} | {r['total_ms']:.2f} | {g['culling']:.2f} | {g['visibility']:.2f} | "
                  f"{g['HiZ']:.2f} | {g['classify']:.2f} | {g['shade']:.2f} | {g['other']:.2f} | "
                  f"{r['wall_ms_per_frame']:.1f} |")
    print()
    print("| Scene | Size | Instances | Visible (phase 1) | Rechecked / disoccluded | Meshlets culled | "
          "Meshlets occluded | Meshlets drawn |")
    print("|---|---|---|---|---|---|---|---|")
    for name in sorted(data):
        c = data[name].get("gpu_driven", {}).get("culling")
        if not c:
            continue
        scene, size = name.split("|")
        print(f"| {scene} | {size} | {c['instances']} | {c['instances_visible']} | "
              f"{c['instances_rechecked']} / {c['instances_disoccluded']} | {c['meshlets_culled']} | "
              f"{c['meshlets_occluded']} | {c['meshlets_drawn'][0]} + {c['meshlets_drawn'][1]} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
