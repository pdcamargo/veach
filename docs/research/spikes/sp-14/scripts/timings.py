#!/usr/bin/env python3
"""Spike SP-14 (VCH-090): GPU ms of the auto-exposure passes and the starfield, over MCP's gpu_timings.

  python3 docs/research/spikes/sp-14/scripts/timings.py --lab build/release/apps/lab/veach-lab [--samples 60]

Each configuration is one headless lab (1920 x 1080) held at one frame of the exposure timeline (VEACH_SP14_HOLD),
driven over MCP: `warmup` frames for the GPU's clocks to settle, then `samples` single frames, each followed by
gpu_timings; the median per pass, then the median over interleaved rounds.
"""
import argparse
import os
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../../tests/mcp"))
from test_mcp_lab import McpClient  # noqa: E402

CONFIGS = [
    ("ground (A), auto-exposure, stars", {"VEACH_SP14_FROM": "100"}),
    ("cave (C), auto-exposure, stars", {"VEACH_SP14_FROM": "600"}),
    ("orbit over the limb (E), auto-exposure, stars", {"VEACH_SP14_FROM": "1200"}),
    ("deep space (G), auto-exposure, stars", {"VEACH_SP14_FROM": "1900"}),
    ("deep space (G), auto-exposure, no stars", {"VEACH_SP14_FROM": "1900", "VEACH_SP14_STARS": "0"}),
]


def measure(lab: str, env: dict[str, str], samples: int, warmup: int) -> dict[str, float]:
    project = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../../apps/lab")
    saved = {k: v for k, v in os.environ.items() if k.startswith("VEACH_SP")}
    for k in saved:
        os.environ.pop(k)
    run_env = {"VEACH_SP14": "exposure", "VEACH_SP14_HOLD": "1", "VEACH_SP14_SIZE": "1920x1080", **env}
    os.environ.update(run_env)
    cwd = os.getcwd()
    os.chdir(project)
    try:
        client = McpClient([lab, "--mcp", "--headless", "--rhi", "metal"])
    finally:
        for k in run_env:
            os.environ.pop(k, None)
        os.environ.update(saved)
        os.chdir(cwd)
    client.initialize()
    client.call_json("step_frames", count=warmup)
    per_pass: dict[str, list[float]] = {}
    for _ in range(samples):
        client.call_json("step_frames", count=1)
        for p in client.call_json("gpu_timings")["passes"]:
            per_pass.setdefault(p["name"], []).append(p["gpu_ms"])
    client.close()
    return {name: statistics.median(values) for name, values in per_pass.items()}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--samples", type=int, default=60)
    parser.add_argument("--warmup", type=int, default=300)
    parser.add_argument("--rounds", type=int, default=2)
    args = parser.parse_args()
    lab = os.path.abspath(args.lab)
    rounds: dict[str, list[dict[str, float]]] = {}
    for _ in range(args.rounds):
        for label, env in CONFIGS:
            result = measure(lab, env, args.samples, args.warmup)
            rounds.setdefault(label, []).append(result)
            print(label, result, file=sys.stderr)
    names = ["shade", "exposure histogram", "exposure resolve", "display", "exposure readback"]
    print("| Configuration | " + " | ".join(names) + " |")
    print("|---|" + "---|" * len(names))
    for label, _ in CONFIGS:
        cells = []
        for n in names:
            values = [r[n] for r in rounds[label] if n in r]
            cells.append(f"{statistics.median(values):.3f}" if values else "")
        print(f"| {label} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
