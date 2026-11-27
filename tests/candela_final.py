#!/usr/bin/env python3
"""Candela's gates on the final image, Performance tier (1280x720 output, the tier's upscaler; docs/specs/candela/spec.md):

  python3 -I tests/candela_final.py --lab <veach-lab> --imgdiff <veach> --scene S flicker --tolerance tests/tolerance.json
  python3 -I tests/candela_final.py --lab <veach-lab> --imgdiff <veach> --scene S light_off [--gi candela|off]
      [--max-frames 10]

flicker (candela.AC4): the mean FLIP between consecutive frames of a still camera, with Candela, may exceed the same
pipeline's with direct light only (its floor: the upscaler's jitter shows on any lit detail) by tests/tolerance.json's
"flicker" margin, unless an exception names an open backlog item. light_off (candela.AC5): after the scene's light is
switched off, the final image's mean luminance goes 90% of the way to where it settles within --max-frames frames.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import candela_step1  # noqa: E402


def flicker_limit(tolerance: dict, scene: str) -> tuple[float, str]:
    section = tolerance["flicker"]
    entry = next(e for e in section["entries"] if e["name"] == scene)
    for exception in entry.get("exceptions", []):
        return exception["limit"], f"exception, {exception['bug']}"
    return section["margin"], "the margin"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lab", required=True)
    parser.add_argument("--imgdiff", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("check", choices=["flicker", "light_off"])
    parser.add_argument("--tolerance", type=pathlib.Path)
    parser.add_argument("--gi", default="candela")
    parser.add_argument("--max-frames", type=int, default=10)
    args = parser.parse_args()
    lab = pathlib.Path(args.lab).resolve()
    if args.scene.startswith("bistro") and not (ROOT / "apps/lab/assets/bistro/BistroExterior.bin").exists():
        print("VEACH_GPU_UNAVAILABLE: skipped (Bistro's assets are not here)")
        return 0
    candela_step1.FINAL.update(size="1280x720", upscaling={"mode": "automatic"})
    candela_step1.VIEW["response"] = "final"
    with tempfile.TemporaryDirectory() as tmp:
        try:
            if args.check == "flicker":
                result = candela_step1.flicker(lab, "metal", pathlib.Path(args.imgdiff), args.scene, pathlib.Path(tmp))
            else:
                result = candela_step1.response(lab, "metal", args.scene, "light_off", pathlib.Path(tmp),
                                                args.max_frames + 10, gi=args.gi)
        except RuntimeError as error:
            if "rhi.backend_unavailable" in str(error):
                print("VEACH_GPU_UNAVAILABLE: skipped (no Metal ray tracing)")
                return 0
            raise
    if args.check == "flicker":
        limit, source = flicker_limit(json.loads(args.tolerance.read_text(encoding="utf-8")), args.scene)
        increase = result["candela"] - result["direct_only"]
        print(f"{args.scene}: flicker {result['candela']:.4f} with Candela, {result['direct_only']:.4f} with direct "
              f"light only: +{increase:.4f} (limit +{limit}: {source})")
        failed = increase > limit + 1e-9
    else:
        frames = result["frames_to_90_percent"]
        print(f"{args.scene}, GI {args.gi}: light off settles (90%) in {frames} frames (limit {args.max_frames})")
        failed = frames is None or frames > args.max_frames
    if failed:
        print(f"candela_final: {args.scene}: {args.check} fails", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
