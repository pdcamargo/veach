#!/usr/bin/env python3
"""Spike SP-16 (VCH-092): the screenshots, crops and image numbers of the write-up.

  python3 docs/research/spikes/sp-16/scripts/shots.py --lab build/release/apps/lab/veach-lab [--out DIR]

For each view at 1920x1080: the visibility buffer (no AA), forward (no AA), forward with a depth prepass and 4x MSAA
with alpha to coverage, and a 16-sample supersampled reference (the visibility buffer at 7680x4320, box-filtered).
FLIP of each against the reference (veach imgdiff), 4x nearest-neighbor crops of foliage edges side by side
(visibility | MSAA | reference), and the shimmer of a slow pan: the mean change between two consecutive frames,
against the reference's (which is the real motion).
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lab  # noqa: E402
import pngio  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SPIKE = os.path.abspath(os.path.join(HERE, ".."))

CONFIGS = {
    "vis": {"VEACH_SP16": "vis"},
    "forward": {"VEACH_SP16": "forward"},
    "msaa": {"VEACH_SP16": "prepass", "VEACH_SP16_MSAA": "4"},
    "reference": {"VEACH_SP16": "vis", "VEACH_SP16_SSAA": "4"},
}
# (x, y) of a 240x135 crop per view, on foliage edges against the sky or the ground.
CROPS = {
    "ground": [(1080, 120), (560, 420)],
    "mid": [(960, 560), (40, 280)],
    "far": [(900, 640), (600, 760)],
}


def flip(veach: str, reference: str, test: str) -> float:
    out = subprocess.run([veach, "imgdiff", reference, test, "--max-mean", "1"], capture_output=True, text=True)
    m = re.search(r"mean FLIP ([0-9.]+)", out.stdout + out.stderr)
    return float(m.group(1)) if m else float("nan")


def crop(path: str, x0: int, y0: int, w: int, h: int, scale: int) -> tuple[int, int, bytearray]:
    width, _, px = pngio.read(path)
    out = bytearray(w * scale * h * scale * 4)
    for y in range(h * scale):
        for x in range(w * scale):
            s = ((y0 + y // scale) * width + x0 + x // scale) * 4
            d = (y * w * scale + x) * 4
            out[d:d + 4] = px[s:s + 4]
    return w * scale, h * scale, out


def side_by_side(images: list[tuple[int, int, bytearray]], gap: int = 8) -> tuple[int, int, bytearray]:
    h = images[0][1]
    w = sum(i[0] for i in images) + gap * (len(images) - 1)
    out = bytearray(b"\xff\xff\xff\xff" * w * h)
    x0 = 0
    for iw, ih, px in images:
        for y in range(ih):
            out[(y * w + x0) * 4:(y * w + x0 + iw) * 4] = px[y * iw * 4:(y + 1) * iw * 4]
        x0 += iw + gap
    return w, h, out


def mean_change(a: str, b: str) -> float:
    _, _, pa = pngio.read(a)
    _, _, pb = pngio.read(b)
    total = sum(abs(pa[i] - pb[i]) for i in range(0, len(pa), 4))  # red channel is enough for a ratio
    return total / (len(pa) / 4) / 255.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--veach", default=None, help="veach-cli (default: next to the lab)")
    parser.add_argument("--out", default=SPIKE)
    parser.add_argument("--views", nargs="*", default=list(CROPS))
    parser.add_argument("--work", default=None, help="where full-size intermediate images go (default: --out)")
    args = parser.parse_args()
    veach = args.veach or os.path.join(os.path.dirname(args.lab), "../../tools/veach-cli/veach")
    work = args.work or args.out
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    for view in args.views:
        paths = {}
        for name, env in CONFIGS.items():
            paths[name] = os.path.join(work, f"{view}_{name}.png")
            r = lab.run(args.lab, {**env, "VEACH_SP16_VIEW": view}, 6, paths[name])
            assert r["exit"] == 0, r["log"][-2000:]
        print(f"{view}: FLIP against the 16-sample reference: "
              + ", ".join(f"{n} {flip(veach, paths['reference'], paths[n]):.4f}" for n in ("vis", "forward", "msaa"))
              + f"; vis against forward {flip(veach, paths['forward'], paths['vis']):.4f}", flush=True)
        for i, (x, y) in enumerate(CROPS[view]):
            row = [crop(paths[n], x, y, 240, 135, 4) for n in ("vis", "msaa", "reference")]
            pngio.write(os.path.join(args.out, f"{view}_crop{i}.png"), *side_by_side(row))
        # Shimmer: two consecutive frames of a slow pan (0.02 degrees a frame).
        change = {}
        for name in ("vis", "msaa", "reference"):
            frames = []
            for n in (40, 41):
                p = os.path.join(work, f"{view}_{name}_pan{n}.png")
                r = lab.run(args.lab, {**CONFIGS[name], "VEACH_SP16_VIEW": view, "VEACH_SP16_PAN": "0.02"}, n, p)
                assert r["exit"] == 0, r["log"][-2000:]
                frames.append(p)
            change[name] = mean_change(*frames)
        print(f"{view}: mean change between two frames of a 0.02 degree pan: "
              + ", ".join(f"{n} {v:.5f}" for n, v in change.items()), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
