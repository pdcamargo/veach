#!/usr/bin/env python3
"""The sun shadows' culling and caching (VCH-143, docs/research/sun-shadows/README.md) against the build before it:
the GPU frame, the shadow passes, the shading and the CPU per frame for Bistro at noon and sunset and the SP-16 field
at 1080p and 1440p, and the slice landed, each also with shadow maps off (SunShadows.suns = 0) on the new build, the
cost's zero line.

  python3 docs/research/sun-shadows/scripts/culling_sweep.py --new <tree> --old <tree> [--rounds 2] [--out f.json]
  python3 docs/research/sun-shadows/scripts/culling_sweep.py --table f.json

<tree> is a source tree with a release build (build/release/apps/{lab,slice}). Runs alternate between the builds,
`rounds` times; the tables take the median over rounds of each run's median (gpu-driven/scripts/timings.py, which
steps frames back to back over MCP so the GPU never idles). The slice is measured unpaced from its landing, without
MCP, and only on the two builds (its frames are CPU-bound).
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../gpu-driven/scripts"))
import timings  # noqa: E402

SIZES = ["1920x1080", "2560x1440"]
LABELS = ("before", "after", "no maps")


def configs(only: list[str]) -> list[dict]:
    out = []
    for size in SIZES:
        for time_of_day in ("noon", "sunset"):
            out.append({"name": f"bistro_{time_of_day}|{size}", "app": "lab",
                        "script": f"scripts/bistro_{time_of_day}.luau", "size": size, "env": {}})
        out.append({"name": f"field_ground|{size}", "app": "lab", "script": None, "size": size,
                    "env": {"VEACH_SP16": "engine", "VEACH_SP16_VIEW": "ground"}, "warmup": 60, "samples": 40})
    for scale in ("s0.01", "s1"):
        out.append({"name": f"slice_{scale}_landed|2560x1440", "app": "slice", "script": f"scripts/slice_{scale}.luau",
                    "size": "2560x1440", "env": {"VEACH_SLICE_AUTOPILOT": "1"}, "until": "autopilot: landed",
                    "samples": 60})
    return [c for c in out if not only or any(c["name"].startswith(o) for o in only)]


def measure_lab(tree: str, c: dict, no_maps: bool) -> dict:
    """timings.measure, with the active camera's SunShadows set to suns = 0 first when `no_maps`."""
    app = os.path.join(tree, "build/release/apps/lab/veach-lab")
    project = os.path.join(tree, "apps/lab")
    warmup, samples = c.get("warmup", 120), c.get("samples", 100)
    client = timings.start(app, project, c["script"], c["size"], {**c["env"],
                                                                   "VEACH_GPU_TIMINGS": f"{samples},{warmup}"})
    if no_maps:
        cameras = client.call_json("query_entities", **{"with": ["Camera"]})
        for camera in cameras:
            client.call_json("set_component", entity=camera["id"], component="SunShadows", value={"suns": 0})
    client.call_json("step_frames", count=max(1, warmup - 2))
    t0 = time.time()
    stepped = 0
    while not any("gpu_timings" in line for line in client.stderr):
        client.call_json("step_frames", count=50)
        stepped += 50
        if stepped > 4 * samples + 200:
            raise RuntimeError("no gpu_timings line:\n" + "".join(client.stderr[-20:]))
    wall_ms = (time.time() - t0) * 1000.0 / stepped
    culling = client.call_json("gpu_timings").get("culling")
    client.close()
    line = next(line for line in client.stderr if "gpu_timings" in line)
    report = json.loads(line.split("gpu_timings ", 1)[1])
    return {"frame_ms": report.get("frame_ms"), "total_ms": report["total_ms"], "passes": report["passes"],
            "wall_ms_per_frame": wall_ms, "culling": culling}


LANDED: dict[str, int] = {}


def run(tree: str, c: dict, no_maps: bool) -> dict:
    if c["app"] == "lab":
        return measure_lab(tree, c, no_maps)
    app = os.path.join(tree, "build/release/apps", c["app"], f"veach-{c['app']}")
    project = os.path.join(tree, "apps", c["app"])
    if c["script"] not in LANDED:
        LANDED[c["script"]] = timings.find_frame(app, project, c["script"], c["size"], c["env"], c["until"])
    return timings.measure_direct(app, project, c["script"], c.get("samples", 100), LANDED[c["script"]], c["env"])


def table(path: str) -> None:
    summary = json.load(open(path, encoding="utf-8"))
    print("| Scene | Size | Build | GPU frame | Shadows add | Shadow passes (sum) | Shade | CPU ms/frame | "
          "Meshlets drawn per cascade |")
    print("|---|---|---|---|---|---|---|---|---|")
    for name in sorted(summary):
        scene, size = name.split("|")
        zero = (summary[name].get("no maps") or {}).get("frame_ms")
        for label in LABELS:
            r = summary[name].get(label)
            if not r:
                continue
            shadow = sum(v for k, v in r["passes"].items() if k.startswith("shadow"))
            frame = r.get("frame_ms")
            added = f"+{frame - zero:.2f}" if frame is not None and zero is not None and label != "no maps" else ""
            drawn = (r.get("culling") or {}).get("shadow_meshlets_drawn")
            print(f"| {scene} | {size} | {label} | {frame:.2f} | {added} | {shadow:.2f} | "
                  f"{r['passes'].get('shade', 0):.2f} | {r['wall_ms_per_frame']:.1f} | "
                  f"{', '.join(str(d) for d in drawn) if drawn else ''} |")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--new")
    parser.add_argument("--old")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                      "../culling_timings.json"))
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
            variants = [("before", args.old, False), ("after", args.new, False)]
            if c["app"] == "lab":
                variants.append(("no maps", args.new, True))
            for label, tree, no_maps in variants:
                try:
                    result = run(tree, c, no_maps)
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
