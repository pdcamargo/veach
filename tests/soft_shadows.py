#!/usr/bin/env python3
"""Soft shadows (docs/specs/soft-shadows/spec.md): a sun, a point light and a spot light with a size over the Cornell
box. shadows.AC1: the real-time renderer's direct light matches the path tracer's (tools/bench/compare_direct.py).
shadows.AC2: each differs from the same light without a size, in both renderers (the penumbra is there).

  python3 soft_shadows.py --lab <veach-lab> --imgdiff <veach-cli> --out <dir> [--rhi metal]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# Sizes are large enough to measure at 320x180 (the sun's real 0.0093 rad would be a pixel wide). {size} is the size
# field, or nothing for the hard-shadow twin.
LIGHTS = {
    "sun": "local l = veach.spawn('test sun')\n"
           "l:set(veach.components.Transform, { translation = vector.zero, "
           "rotation = veach.quat.look_rotation(vector.create(0.4, -1, -0.3)), scale = vector.one })\n"
           "l:set(veach.components.DirectionalLight, { color = vector.one, illuminance = 40{size} })",
    "point": "local l = veach.spawn('test bulb')\n"
             "l:set(veach.components.Transform, { translation = vector.create(0.6, 1.5, 0.6), "
             "rotation = veach.quat.identity(), scale = vector.one })\n"
             "l:set(veach.components.PointLight, { color = vector.one, intensity = 25{size} })",
    "spot": "local l = veach.spawn('test spot')\n"
            "l:set(veach.components.Transform, { translation = vector.create(-0.7, 1.6, 0.8), "
            "rotation = veach.quat.look_rotation(vector.create(0.6, -1, -0.6)), scale = vector.one })\n"
            "l:set(veach.components.SpotLight, { color = vector.one, intensity = 40, inner_angle = 0.35, "
            "outer_angle = 0.6{size} })",
}
SIZES = {"sun": ", angle = 0.2", "point": ", radius = 0.2", "spot": ", radius = 0.15"}


def light_script(name: str, sized: bool) -> str:
    return LIGHTS[name].replace("{size}", SIZES[name] if sized else "")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--imgdiff", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--rhi", default="metal")
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    failed = False

    def compare(label: str, extra: str) -> float:
        result = subprocess.run(
            [sys.executable, "-I", str(ROOT / "tools" / "bench" / "compare_direct.py"), "--rhi", args.rhi,
             "--lab", args.lab, "--imgdiff", args.imgdiff, "--out", str(out), "--size", "320x180", "--frames", "32",
             "--spf", "16", "--realtime-frames", "256", "--no-reuse", "--candidates", "32", "--label", label,
             "--extra", extra, str(ROOT / "apps" / "lab" / "scripts" / "cornell_box.luau")],
            capture_output=True, text=True)
        if "rhi.backend_unavailable" in result.stdout + result.stderr:
            print("VEACH_GPU_UNAVAILABLE: skipped")
            sys.exit(0)
        match = re.search(rf"\| cornell_box_{label} \| ([\d.]+) \|", result.stdout)
        if not match:
            raise RuntimeError(f"compare_direct failed for {label}:\n{result.stdout}\n{result.stderr}")
        return float(match[1])

    def flip(a: pathlib.Path, b: pathlib.Path) -> float:
        text = subprocess.run([args.imgdiff, "imgdiff", str(a), str(b), "--max-mean", "1", "--max-p999", "1",
                               "--max-outliers", "1000000000"], capture_output=True, text=True).stdout
        return float(text.split("mean FLIP ")[1].split()[0])

    for name in LIGHTS:
        soft = compare(f"{name}_soft", light_script(name, True))
        compare(f"{name}_hard", light_script(name, False))
        ok = soft <= 0.04
        print(f"{'ok' if ok else 'FAIL'}: [shadows.AC1] {name}: real time against the path tracer, FLIP {soft:.4f} <= 0.04")
        failed |= not ok
        for renderer in ("realtime", "reference"):
            penumbra = flip(out / f"cornell_box_{name}_soft.{renderer}.png", out / f"cornell_box_{name}_hard.{renderer}.png")
            ok = penumbra >= 0.005
            print(f"{'ok' if ok else 'FAIL'}: [shadows.AC2] {name}: the {renderer} image changes with the size "
                  f"(FLIP {penumbra:.4f} >= 0.005)")
            failed |= not ok
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
