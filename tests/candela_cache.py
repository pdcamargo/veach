#!/usr/bin/env python3
"""Candela's radiance cache alone (Candela.gather = "surface_cache", docs/specs/candela/spec.md): a regression gate
on how steady and how quick it is, so a change to the cache cannot make it worse unnoticed.

  python3 -I tests/candela_cache.py --lab <veach-lab> --imgdiff <veach> --scene cornell_box_realtime \\
      --max-flicker 0.013 --max-light-off 11

Flicker: the mean FLIP between consecutive final frames of a still camera (tools/bench/candela_step1.py). Light off:
frames until the cache's indirect light has gone 90% of the way after a light is switched off. The limits are the
cache's measured values with a margin, not its targets (0.005 and 10 frames, which it does not meet yet: VCH-067).
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "bench"))
import candela_step1  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lab", required=True)
    parser.add_argument("--imgdiff", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--max-flicker", type=float, required=True)
    parser.add_argument("--max-light-off", type=int, required=True)
    args = parser.parse_args()
    lab = pathlib.Path(args.lab).resolve()
    if args.scene.startswith("bistro") and not (ROOT / "apps/lab/assets/bistro/BistroExterior.bin").exists():
        print("VEACH_GPU_UNAVAILABLE: skipped (Bistro's assets are not here)")
        return 0
    candela_step1.CANDELA.update({"gather": "surface_cache"})
    with tempfile.TemporaryDirectory() as tmp:
        try:
            flicker = candela_step1.flicker(lab, "metal", pathlib.Path(args.imgdiff), args.scene, pathlib.Path(tmp))
        except RuntimeError as error:
            if "rhi.backend_unavailable" in str(error):
                print("VEACH_GPU_UNAVAILABLE: skipped (no Metal ray tracing)")
                return 0
            raise
        light_off = candela_step1.response(lab, "metal", args.scene, "light_off", pathlib.Path(tmp),
                                           args.max_light_off + 10)
    frames = light_off["frames_to_90_percent"]
    print(f"{args.scene}: flicker {flicker['candela']:.4f} (limit {args.max_flicker}), light off {frames} frames "
          f"(limit {args.max_light_off})")
    failed = flicker["candela"] > args.max_flicker or frames is None or frames > args.max_light_off
    if failed:
        print(f"candela_cache: {args.scene}: the cache is less steady or slower than its recorded values",
              file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
