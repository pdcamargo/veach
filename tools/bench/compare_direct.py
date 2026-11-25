#!/usr/bin/env python3
"""The real-time renderer's direct lighting against the path tracer's (docs/specs/realtime-renderer-v0/spec.md,
rt.AC8): the start of the Candela groundwork.

  python3 -I tools/bench/compare_direct.py [--preset debug] [--rhi metal] [--size 640x360] [--frames 64]
                                           [--spf 16] [--max-mean X] apps/lab/scripts/cornell_box.luau ...

For each lab scene script (which must name its camera entity `camera`), two temporary variants are written next
to it and rendered headless at --size:
  - real time: the camera's PathTracer removed, RealtimeLighting with the ambient stopgap off (direct light only),
    no upscaler (Upscaling.mode = none: one frame at full resolution, no jitter);
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
import struct
import sys
import uuid
import zlib

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


def mean_luminance(png: pathlib.Path) -> float:
    """Mean linear luminance of an 8-bit RGB(A) PNG (what screenshots write), decoded from sRGB."""
    data = png.read_bytes()
    pos, idat, width, height, channels = 8, b"", 0, 0, 4
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, _, color = struct.unpack(">IIBB", chunk[:10])
            channels = 4 if color == 6 else 3
        elif kind == b"IDAT":
            idat += chunk
    raw = zlib.decompress(idat)
    stride = width * channels
    linear = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in (i / 255.0 for i in range(256))]
    total, previous, i = 0.0, bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = previous[x]
            c = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(0, stride, channels):
            total += 0.2126 * linear[line[x]] + 0.7152 * linear[line[x + 1]] + 0.0722 * linear[line[x + 2]]
        previous = line
    return total / (width * height)


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
    parser.add_argument("--candidates", type=int, default=12, help="ReSTIR DI's initial candidates per pixel")
    parser.add_argument("--final", action="store_true",
                        help="the final image instead: denoised, after --realtime-frames of warm-up (temporal.AC6)")
    parser.add_argument("--render-scale", type=float, default=0.75, help="with --final and an upscaler")
    parser.add_argument("--upscaler", default="none", choices=["none", "automatic", "temporal", "metalfx"],
                        help="with --final: none compares full-resolution pixel centers like v0; an upscaler compares "
                             "its anti-aliased image with an anti-aliased reference")
    parser.add_argument("--max-luminance-error", type=float, default=None,
                        help="fail if the mean luminance differs from the reference's by more than this fraction")
    parser.add_argument("--sky", action="store_true",
                        help="direct light plus sky light: the scene's sky, the ambient stopgap on with unlimited range, the path tracer's "
                             "mode 3 (physical-sky spec, sky.AC3); otherwise both sides render without a sky")
    parser.add_argument("--no-reuse", action="store_true",
                        help="no temporal or spatial reservoir reuse: independent frames converge faster when averaged")
    parser.add_argument("--realtime-frames", type=int, default=1,
                        help="frames of real-time lighting averaged (RealtimeLighting.view = accumulated) before comparing")
    parser.add_argument("--max-mean", type=float, default=None)
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "build" / "compare_direct")
    parser.add_argument("--lab", type=pathlib.Path, help="veach-lab (default: build/<preset>/apps/lab/veach-lab)")
    parser.add_argument("--imgdiff", type=pathlib.Path, help="veach-cli (default: build/<preset>/tools/veach-cli/veach)")
    parser.add_argument("--extra", default="", help="Luau appended to both variants (e.g. a light)")
    parser.add_argument("--label", default="", help="suffix for the scene's name in the report and file names")
    args = parser.parse_args()
    size = tuple(int(v) for v in args.size.split("x"))
    lab = args.lab or ROOT / "build" / args.preset / "apps" / "lab" / "veach-lab"
    imgdiff = args.imgdiff or ROOT / "build" / args.preset / "tools" / "veach-cli" / "veach"
    args.out.mkdir(parents=True, exist_ok=True)

    failed = False
    print(f"| Scene | Mean FLIP | 99.9th percentile | Pixels above 0.5 | Mean luminance |  ({args.size}, reference "
          f"{args.frames * args.spf} spp, real time {args.candidates} candidates, {args.realtime_frames} frames)")
    print("|---|---|---|---|---|")
    view = "final" if args.final else "accumulated" if args.realtime_frames > 1 else "noisy"
    no_sky = "" if args.sky else "camera:set(veach.components.Sky, { mode = \"none\" })\n"
    for scene in args.scenes:
        scene = scene.resolve()
        name = scene.stem + (f"_{args.label}" if args.label else "")
        realtime = variant(scene, f"{args.label}realtime", size, args.extra + "\n"
                           "camera:remove(veach.components.PathTracer)\n" + no_sky +
                           "camera:set(veach.components.RealtimeLighting, { light_candidates = "
                           f"{args.candidates}, ambient = {'true' if args.sky else 'false'}, "
                           f"ao_rays = {4 if args.sky else 0}, ao_radius = {10000 if args.sky else 1}, "
                           f"denoise = {'true' if args.final else 'false'}, view = \"{view}\", "
                           f"temporal_reuse = {'false' if args.no_reuse else 'true'}, "
                           f"spatial_samples = {0 if args.no_reuse else 2} }})\n"
                           # Full resolution through pixel centers like the reference: no jitter, no upscaler (or,
                           # for the final image, the default upscaler against an anti-aliased reference).
                           + (f"camera:set(veach.components.Upscaling, {{ mode = \"{args.upscaler}\", "
                              f"render_scale = {args.render_scale} }})"
                              if args.final and args.upscaler != "none" else
                              "camera:set(veach.components.Upscaling, { mode = \"none\", render_scale = 1 })"))
        reference = variant(scene, f"{args.label}reference", size, args.extra + "\n" + no_sky +
                            "camera:set(veach.components.PathTracer, { samples_per_frame = "
                            f"{args.spf}, max_bounces = 1, mode = {3 if args.sky else 1}, "
                            f"jitter = {'true' if args.final and args.upscaler != 'none' else 'false'} }})")
        try:
            rt_png = args.out / f"{name}.realtime.png"
            pt_png = args.out / f"{name}.reference.png"
            render(lab, args.rhi, realtime, max(3, args.realtime_frames), rt_png)
            render(lab, args.rhi, reference, args.frames, pt_png)
            diff = subprocess.run([str(imgdiff), "imgdiff", str(pt_png), str(rt_png), "--max-mean", "1",
                                   "--max-p999", "1", "--max-outliers", "1000000000",
                                   "--diff", str(args.out / f"{name}.diff.png")],
                                  capture_output=True, text=True).stdout
            match = re.search(r"mean FLIP ([\d.]+).*?99\.9th percentile ([\d.]+).*?(\d+) pixels above 0\.5", diff)
            if not match:
                raise RuntimeError(f"{scene.name}: unexpected imgdiff output: {diff.strip()}")
            mean, p999, outliers = float(match[1]), float(match[2]), int(match[3])
            luminance = mean_luminance(rt_png) / max(mean_luminance(pt_png), 1e-9) - 1.0
            print(f"| {name} | {mean:.4f} | {p999:.3f} | {outliers} | {luminance:+.2%} |")
            if args.max_mean is not None and mean > args.max_mean:
                print(f"compare_direct: {name}: mean FLIP {mean:.4f} exceeds {args.max_mean}", file=sys.stderr)
                failed = True
            if args.max_luminance_error is not None and abs(luminance) > args.max_luminance_error:
                print(f"compare_direct: {name}: mean luminance off by {luminance:+.2%}, more than "
                      f"{args.max_luminance_error:.0%}", file=sys.stderr)
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
