#!/usr/bin/env python3
"""Spike SP-10 (VCH-086): GPU ms per render-graph pass for each atmosphere configuration, over MCP's gpu_timings.

  python3 docs/research/spikes/sp-10/scripts/timings.py --lab build/release/apps/lab/veach-lab [--samples 40]

Each configuration is one headless lab (1920 x 1080) driven over MCP: one frame (the LUTs that depend only on the
medium are built then), `warmup` frames for the GPU's clocks to settle, then `samples` single frames, each followed by gpu_timings; the median per pass is printed.
"""
import argparse
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../../tests/mcp"))
from test_mcp_lab import McpClient  # noqa: E402

CONFIGS = [
    ("Sky component (ADR 0038 path), ground", {"VEACH_SP10": "legacy"}),
    ("planet, ground, 1 sun (LUT)", {"VEACH_SP10": "earth"}),
    ("planet, ground, 1 sun (LUT), medium rebuilt every frame", {"VEACH_SP10": "earth", "VEACH_SP10_REBUILD": "1"}),
    ("planet, ground, 2 suns (LUT)", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "wide"}),
    ("planet, ground, 4 suns (LUT)", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "four"}),
    ("planet, ground, 1 sun + eclipse (LUT)", {"VEACH_SP10": "earth", "VEACH_SP10_ECLIPSE": "0.5"}),
    ("planet, ground, 1 sun, march forced", {"VEACH_SP10": "earth", "VEACH_SP10_MARCH": "always"}),
    ("planet, ground, 2 suns, march forced", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "wide",
                                              "VEACH_SP10_MARCH": "always"}),
    ("planet, just over the top, 1 sun (march)", {"VEACH_SP10": "earth", "VEACH_SP10_ALTITUDE": "top+"}),
    ("planet, just over the top, 1 sun (march, 16 steps)", {"VEACH_SP10": "earth", "VEACH_SP10_ALTITUDE": "top+",
                                                            "VEACH_SP10_STEPS": "16"}),
    ("planet, orbit, 1 sun (march)", {"VEACH_SP10": "earth", "VEACH_SP10_ALTITUDE": "orbit"}),
    ("planet, orbit, 1 sun (march, 16 steps)", {"VEACH_SP10": "earth", "VEACH_SP10_ALTITUDE": "orbit",
                                                "VEACH_SP10_STEPS": "16"}),
    ("planet, orbit, 2 suns (march)", {"VEACH_SP10": "earth", "VEACH_SP10_ALTITUDE": "orbit",
                                       "VEACH_SP10_SUNS": "wide"}),
    ("planet, orbit, 1 sun + eclipse (march)", {"VEACH_SP10": "earth", "VEACH_SP10_ALTITUDE": "orbit",
                                                "VEACH_SP10_ECLIPSE": "0"}),
    ("Mars s = 0.01, orbit, 1 sun (march)", {"VEACH_SP10": "mars", "VEACH_SP10_SCALE": "0.01",
                                              "VEACH_SP10_ALTITUDE": "orbit"}),
]


def measure(lab: str, env: dict[str, str], samples: int, warmup: int) -> tuple[dict[str, float], dict[str, float]]:
    run_env = {k: v for k, v in os.environ.items() if not k.startswith("VEACH_SP10")}
    run_env.update(env)
    run_env["VEACH_SP10_SIZE"] = "1920x1080"
    project = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../../apps/lab")
    cwd = os.getcwd()
    os.chdir(project)
    try:
        os.environ.update(run_env)
        client = McpClient([lab, "--mcp", "--headless", "--rhi", "metal"])
    finally:
        for key in env:
            os.environ.pop(key, None)
        os.environ.pop("VEACH_SP10_SIZE", None)
        os.chdir(cwd)
    client.initialize()
    client.call_json("step_frames", count=3)
    first = {p["name"]: p["gpu_ms"] for p in client.call_json("gpu_timings")["passes"]}
    client.call_json("step_frames", count=warmup)
    per_pass: dict[str, list[float]] = {}
    for _ in range(samples):
        client.call_json("step_frames", count=1)
        for p in client.call_json("gpu_timings")["passes"]:
            per_pass.setdefault(p["name"], []).append(p["gpu_ms"])
    client.close()
    return first, {name: statistics.median(values) for name, values in per_pass.items()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--samples", type=int, default=40)
    parser.add_argument("--warmup", type=int, default=300)
    parser.add_argument("--rounds", type=int, default=2)
    args = parser.parse_args()
    lab = os.path.abspath(args.lab)
    results = []
    # Rounds interleave the configurations, so a change of GPU clock hits all of them; the median of the rounds.
    rounds: dict[str, list[dict[str, float]]] = {}
    firsts: dict[str, dict[str, float]] = {}
    for _ in range(args.rounds):
        for label, env in CONFIGS:
            first, median = measure(lab, env, args.samples, args.warmup)
            rounds.setdefault(label, []).append(median)
            firsts[label] = first
            print(label, json.dumps(median), file=sys.stderr)
    for label, _ in CONFIGS:
        names = {n for r in rounds[label] for n in r}
        results.append((label, firsts[label],
                        {n: statistics.median(r[n] for r in rounds[label] if n in r) for n in names}))
    names = []
    for _, first, median in results:
        for n in list(first) + list(median):
            if n not in names:
                names.append(n)
    print("| Configuration | " + " | ".join(names) + " |")
    print("|---|" + "---|" * len(names))
    for label, first, median in results:
        cells = []
        for n in names:
            if n in median:
                cells.append(f"{median[n]:.3f}")
            elif n in first:
                cells.append(f"({first[n]:.3f})")
            else:
                cells.append("")
        print(f"| {label} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
