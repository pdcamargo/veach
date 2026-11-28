#!/usr/bin/env python3
"""Spike SP-14 (VCH-090): float32 depth precision from the cockpit to the gas giant, computed exactly in float32.

  python3 docs/research/spikes/sp-14/scripts/precision.py

For each distance z: the step between adjacent float32 values of a camera-relative position (z itself); the depth
step of reverse-Z with an infinite far plane (d = near / z, ADR 0013): the range of distances that store the same
float32 depth, one ulp of d mapped back to meters; the same for conventional float32 depth (d = 1 - near / z, far at
infinity; "never" once every farther distance stores 1.0); and one pixel's footprint at 1080p with a 60 degree vertical
field of view. Near plane 0.05 m (ADR 0063's cockpit).
"""
import math
import struct


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def ulp(x: float) -> float:
    """The gap from float32 x to the next float32 away from zero."""
    bits = struct.unpack("<I", struct.pack("<f", f32(x)))[0]
    return abs(struct.unpack("<f", struct.pack("<I", bits + 1))[0] - f32(x))


def bucket(z: float, depth, inverse) -> float:
    """The range of distances around z that store the same float32 depth: the depth's ulp mapped back to meters."""
    d = f32(depth(z))
    a, b = inverse(d), inverse(d + ulp(d))
    return abs(b - a) if math.isfinite(a) and math.isfinite(b) else math.inf


def main() -> None:
    near = 0.05
    pixel = 2.0 * math.tan(math.radians(30.0)) / 1080.0
    rows = [("cockpit sensor housing", 0.1), ("dashboard", 0.5), ("window frame", 1.0), ("cockpit wall", 2.0),
            ("a building", 100.0), ("a mountain", 1e4), ("100 km", 1e5), ("moon at s = 0.01", 3.84e6),
            ("gas giant at s = 0.01", 7e6), ("ADR 0063's 1e8 m", 1e8), ("moon", 3.84e8), ("gas giant", 7e8),
            ("far moon", 9.5e8), ("1 AU", 1.496e11)]
    print("| | distance (m) | float32 position step (m) | reverse-Z depth step (m) | relative | "
          "conventional float depth step (m) | 1080p pixel footprint (m) | pixel / depth step |")
    print("|---|---|---|---|---|---|---|---|")
    for name, z in rows:
        reverse = bucket(z, lambda x: near / x, lambda d: near / d)
        conventional = bucket(z, lambda x: 1.0 - near / x, lambda d: near / (1.0 - d) if d < 1.0 else math.inf)
        footprint = z * pixel
        conv = "never" if math.isinf(conventional) else f"{conventional:.3g}"
        print(f"| {name} | {z:.3g} | {ulp(z):.3g} | {reverse:.3g} | {reverse / z:.2g} | {conv} | {footprint:.3g} | "
              f"{footprint / reverse:.3g} |")


if __name__ == "__main__":
    main()
