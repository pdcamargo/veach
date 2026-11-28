#!/usr/bin/env python3
"""Spike SP-10 (VCH-086): FLIP the image pairs of the findings with `veach imgdiff`.

  python3 compare.py --veach build/release/tools/veach-cli/veach --dir docs/research/spikes/sp-10
"""
import argparse
import os
import re
import subprocess
import sys


def pairs() -> list[tuple[str, str, str]]:
    out = []
    for c in ["earth_s1", "earth_s0.01", "mars_s1", "mars_s0.01"]:
        out.append((f"{c}: just under the top, LUT vs march", f"seam/{c}_top-_never", f"seam/{c}_top-_always"))
        out.append((f"{c}: under the top (LUT) vs over it (march)", f"{c}_top-", f"{c}_top+"))
        out.append((f"{c}: ground, LUT vs march", f"seam/{c}_ground_never", f"seam/{c}_ground_always"))
    for c in ["earth", "mars"]:
        out.append((f"{c}: s = 1 vs 0.01, ground", f"{c}_s1_ground", f"{c}_s0.01_ground"))
        out.append((f"{c}: s = 1 vs 0.01, same fraction of the atmosphere (10 km vs 1 km)", f"{c}_s1_10km",
                    f"{c}_s0.01_1km"))
        out.append((f"{c}: s = 1 vs 0.01, the top (from inside)", f"{c}_s1_top-", f"{c}_s0.01_top-"))
        out.append((f"{c}: s = 1 vs 0.01, orbit", f"{c}_s1_orbit", f"{c}_s0.01_orbit"))
        out.append((f"{c}: s = 1 vs 0.01, sunset", f"sunset/{c}_s1", f"sunset/{c}_s0.01"))
    for e in ["1", "0.75"]:
        out.append((f"earth: s = 1 vs 0.01 with s_a = s_p^{e}, ground", "earth_s1_ground", f"sa/earth_e{e}_ground"))
        out.append((f"earth: s = 1 vs 0.01 with s_a = s_p^{e}, orbit", "earth_s1_orbit", f"sa/earth_e{e}_orbit"))
        out.append((f"earth: s = 1 vs 0.01 with s_a = s_p^{e}, sunset", "sunset/earth_s1", f"sa/earth_e{e}_sunset"))
    for scale in ["1", "0.01"]:
        for name in ["top+", "orbit"]:
            out.append((f"earth_s{scale}: march with 16 steps vs 32, {name}", f"earth_s{scale}_{name}",
                        f"steps/earth_s{scale}_{name}_16"))
    out.append(("Earth from composition vs Hillaire's Earth, ground", "compare/hillaire_s1_ground",
                "compare/earth_s1_ground"))
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--veach", required=True)
    parser.add_argument("--dir", required=True)
    args = parser.parse_args()
    print("| Pair | mean FLIP | p99.9 |")
    print("|---|---|---|")
    for label, a, b in pairs():
        result = subprocess.run([args.veach, "imgdiff", os.path.join(args.dir, a + ".png"),
                                 os.path.join(args.dir, b + ".png"), "--max-mean", "1", "--max-p999", "1",
                                 "--max-outliers", "100000000"], capture_output=True, text=True)
        text = result.stdout + result.stderr
        mean = re.search(r"mean FLIP ([0-9.]+)", text)
        p999 = re.search(r"percentile ([0-9.]+)", text)
        print(f"| {label} | {mean.group(1) if mean else text.strip()} | {p999.group(1) if p999 else ''} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
