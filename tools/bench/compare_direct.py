#!/usr/bin/env python3
"""The real-time renderer's direct lighting against the path tracer's (docs/specs/realtime-renderer-v0/spec.md,
rt.AC8): the start of the Candela groundwork.

  python3 -I tools/bench/compare_direct.py [--preset debug] [--rhi metal] [--size 640x360] [--frames 64]
                                           [--spf 16] [--max-mean X] apps/lab/scripts/cornell_box.luau ...

For each lab scene script (which must name its camera entity `camera`), two temporary variants are written next
to it and rendered headless at --size:
  - real time: the camera's PathTracer removed, RealtimeLighting with the ambient stopgap off (direct light only);
  - reference: the path tracer in direct mode (PathTracer.mode = 1: first-bounce light, no sky), --frames x --spf
    samples per pixel, through pixel centers (jitter off) like the real-time renderer, so edges compare equal
    and the difference is lighting.
Both go through the same exposure and display transform, then `veach imgdiff` reports mean FLIP, the 99.9th
percentile and the pixels above 0.5. Exits 1 if a scene's mean exceeds --max-mean (when given) or a render fails.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[2]
LAB_PROJECT = ROOT / "apps" / "lab"


def variant(script: pathlib.Path, suffix: str, size: tuple[int, int], extra: str) -> pathlib.Path:
    text = script.read_text(encoding="utf-8")
    text = re.sub(r"width = \d+, height = \d+", f"width = {size[0]}, height = {size[1]}", text)
    out = script.with_name(f"zz_{script.stem}_{suffix}.luau")
    out.write_text(text + "\n-- compare_direct.py\n" + extra + "\n", encoding="utf-8")
    meta = out.with_name(out.name + ".meta")
    meta.write_text('{\n  "id": "%s",\n  "loader": "luau-script",\n  "max_texture_size": 0,\n  "version": 1\n}\n'
                    % uuid.uuid4(), encoding="utf-8")
    return out


def render(lab: pathlib.Path, rhi: str, script: pathlib.Path, frames: int, out: pathlib.Path) -> None:
    result = subprocess.run([str(lab), "--rhi", rhi, "--headless", "--frames", str(frames), "--script",
                             f"scripts/{script.name}", "--screenshot", str(out)],
                            cwd=LAB_PROJECT, capture_output=True, text=True)
    if result.returncode != 0:
        errors = [line for line in (result.stdout + result.stderr).splitlines() if " error " in line]
        raise RuntimeError(f"{script.name}: exit {result.returncode}: " + " | ".join(errors[:3]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="+", type=pathlib.Path)
    parser.add_argument("--preset", default="debug")
    parser.add_argument("--rhi", default="metal")
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--frames", type=int, default=64, help="path tracer frames (samples = frames x spf)")
    parser.add_argument("--spf", type=int, default=16, help="path tracer samples per frame")
    parser.add_argument("--emissive-samples", type=int, default=4, help="real-time emissive light samples")
    parser.add_argument("--max-mean", type=float, default=None)
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "build" / "compare_direct")
    args = parser.parse_args()
    size = tuple(int(v) for v in args.size.split("x"))
    lab = ROOT / "build" / args.preset / "apps" / "lab" / "veach-lab"
    imgdiff = ROOT / "build" / args.preset / "tools" / "veach-cli" / "veach"
    args.out.mkdir(parents=True, exist_ok=True)

    failed = False
    print(f"| Scene | Mean FLIP | 99.9th percentile | Pixels above 0.5 |  ({args.size}, reference "
          f"{args.frames * args.spf} spp, real time {args.emissive_samples} emissive samples)")
    print("|---|---|---|---|")
    for scene in args.scenes:
        scene = scene.resolve()
        realtime = variant(scene, "realtime", size,
                           "camera:remove(veach.components.PathTracer)\n"
                           "camera:set(veach.components.RealtimeLighting, { emissive_samples = "
                           f"{args.emissive_samples}, ambient = false, sky = vector.zero, ao_rays = 0, "
                           "ao_radius = 1 })")
        reference = variant(scene, "reference", size,
                            "camera:set(veach.components.PathTracer, { samples_per_frame = "
                            f"{args.spf}, max_bounces = 1, sky = vector.zero, mode = 1, jitter = false }})")
        try:
            rt_png = args.out / f"{scene.stem}.realtime.png"
            pt_png = args.out / f"{scene.stem}.reference.png"
            render(lab, args.rhi, realtime, 3, rt_png)
            render(lab, args.rhi, reference, args.frames, pt_png)
            diff = subprocess.run([str(imgdiff), "imgdiff", str(pt_png), str(rt_png), "--max-mean", "1",
                                   "--max-p999", "1", "--max-outliers", "1000000000",
                                   "--diff", str(args.out / f"{scene.stem}.diff.png")],
                                  capture_output=True, text=True).stdout
            match = re.search(r"mean FLIP ([\d.]+).*?99\.9th percentile ([\d.]+).*?(\d+) pixels above 0\.5", diff)
            if not match:
                raise RuntimeError(f"{scene.name}: unexpected imgdiff output: {diff.strip()}")
            mean, p999, outliers = float(match[1]), float(match[2]), int(match[3])
            print(f"| {scene.stem} | {mean:.4f} | {p999:.3f} | {outliers} |")
            if args.max_mean is not None and mean > args.max_mean:
                print(f"compare_direct: {scene.stem}: mean FLIP {mean:.4f} exceeds {args.max_mean}", file=sys.stderr)
                failed = True
        except RuntimeError as error:
            print(f"compare_direct: {error}", file=sys.stderr)
            failed = True
        finally:
            for path in (realtime, reference):
                path.unlink(missing_ok=True)
                path.with_name(path.name + ".meta").unlink(missing_ok=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
