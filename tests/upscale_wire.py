#!/usr/bin/env python3
"""Thin geometry through the upscalers (ADR 0049, VCH-068): Bistro noon at 2560x1440, rendered natively and at 0.5
render scale with each upscaler; a string-light wire against the sky must stay continuous.

  python3 -I tests/upscale_wire.py --lab build/release/apps/lab/veach-lab --out build/upscale_wire \\
      [--upscalers metalfx,temporal] [--require metalfx]

The wire is found in the native image: along a stretch where it crosses open sky, each column's darkest pixel near
the expected line, and its contrast with the sky around it. An upscaled image keeps the wire in a column where the
same spot is at least 40% as dark against its sky. Reported per upscaler: coverage (columns kept) and the longest gap
(consecutive columns lost). An upscaler in --require fails the test below 95% coverage or with a gap over 3 columns.
macOS only (MetalFX, sips); skipped without Bistro's assets.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
SIZE = (2560, 1440)
# The wire between the rooftops at the top right, against the sky (2560x1440 pixels): a line from START to END,
# searched WINDOW pixels above and below; the chimney and the bulb lie outside this stretch.
START, END, WINDOW = (2360, 253), (2460, 212), 8
CROP = (2340, 190, 140, 90)  # x, y, width, height: holds the stretch with its window


class Mcp:
    def __init__(self, command: list[str]):
        self.proc = subprocess.Popen(command, cwd=ROOT / "apps" / "lab", stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
        self.stderr: list[str] = []
        threading.Thread(target=lambda: self.stderr.extend(self.proc.stderr), daemon=True).start()
        self.next_id = 0

    def call(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method,
                                          "params": params or {}}) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("the lab exited:\n" + "".join(self.stderr[-20:]))
        response = json.loads(line)
        if "error" in response:
            raise RuntimeError(str(response["error"]))
        return response["result"]

    def tool(self, name: str, **arguments) -> dict:
        result = self.call("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise RuntimeError(f"{name}: {result['content'][-1]['text']}")
        return result

    def close(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=120)


def render(lab: str, upscaling: dict, path: pathlib.Path, frames: int) -> None:
    mcp = Mcp([lab, "--mcp", "--headless", "--rhi", "metal", "--script", "scripts/bistro_noon.luau"])
    mcp.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                            "clientInfo": {"name": "upscale_wire", "version": "1"}})
    mcp.tool("step_frames", count=1)
    window = json.loads(mcp.tool("get_component", entity="primary_window", component="Window")["content"][0]["text"])
    window.update(width=SIZE[0], height=SIZE[1])
    mcp.tool("set_component", entity="primary_window", component="Window", value=window)
    # The render scale is the test's, not the quality tier's (ADR 0049).
    mcp.tool("set_component", entity="camera", component="RenderQuality", value={"tier": "custom"})
    mcp.tool("set_component", entity="camera", component="Upscaling", value=upscaling)
    mcp.tool("step_frames", count=frames)
    mcp.tool("screenshot", path=str(path))
    mcp.close()


def luminance_crop(png: pathlib.Path) -> list[list[float]]:
    """The crop's sRGB-decoded luminance, row by row (sips crops; the rest is a minimal PNG decoder)."""
    x, y, w, h = CROP
    cropped = png.with_suffix(".crop.png")
    subprocess.run(["sips", "-c", str(h), str(w), "--cropOffset", str(y), str(x), str(png), "--out", str(cropped)],
                   check=True, capture_output=True)
    import struct
    import zlib
    data = cropped.read_bytes()
    pos, idat = 8, b""
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
    rows, previous, i = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for k in range(stride):
            a = line[k - channels] if k >= channels else 0
            b = previous[k]
            c = previous[k - channels] if k >= channels else 0
            if kind == 1:
                line[k] = (line[k] + a) & 255
            elif kind == 2:
                line[k] = (line[k] + b) & 255
            elif kind == 3:
                line[k] = (line[k] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[k] = (line[k] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([0.2126 * linear[line[k]] + 0.7152 * linear[line[k + 1]] + 0.0722 * linear[line[k + 2]]
                     for k in range(0, stride, channels)])
        previous = line
    return rows


def wire_columns(native: list[list[float]]) -> list[tuple[int, int, float]]:
    """(column, row, contrast) of the wire in the native crop, where it stands out against the sky."""
    out = []
    for x in range(START[0], END[0] + 1):
        expected = START[1] + (END[1] - START[1]) * (x - START[0]) / (END[0] - START[0])
        cx = x - CROP[0]
        rows = range(round(expected) - WINDOW - CROP[1], round(expected) + WINDOW + 1 - CROP[1])
        values = sorted(native[r][cx] for r in rows)
        darkest = min(rows, key=lambda r: native[r][cx])
        sky = values[len(values) // 2]
        contrast = sky - native[darkest][cx]
        if contrast > 0.1 * sky:
            out.append((cx, darkest, contrast))
    return out


def kept(image: list[list[float]], wire: list[tuple[int, int, float]]) -> list[bool]:
    result = []
    for cx, row, contrast in wire:
        column = [image[r][cx] for r in range(row - WINDOW, row + WINDOW + 1)]
        sky = sorted(column)[len(column) // 2]
        darkest = min(image[r][cx] for r in range(row - 1, row + 2))
        result.append(sky - darkest >= 0.4 * contrast)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lab", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--upscalers", default="metalfx,temporal")
    parser.add_argument("--require", default="metalfx")
    parser.add_argument("--frames", type=int, default=64)
    args = parser.parse_args()
    args.lab = str(pathlib.Path(args.lab).resolve())
    if not (ROOT / "apps" / "lab" / "assets" / "bistro" / "BistroExterior.bin").exists():
        print("VEACH_GPU_UNAVAILABLE: skipped (Bistro's assets are not here)")
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    native_png = args.out / "native.png"
    try:
        render(args.lab, {"mode": "temporal", "render_scale": 1.0}, native_png, args.frames)
    except RuntimeError as error:
        if "rhi.backend_unavailable" in str(error):
            print("VEACH_GPU_UNAVAILABLE: skipped (no Metal ray tracing)")
            return 0
        raise
    wire = wire_columns(luminance_crop(native_png))
    if len(wire) < 0.8 * (END[0] - START[0]):
        print(f"upscale_wire: found the wire in only {len(wire)} columns of the native image: has the view moved?")
        return 1
    failed = False
    print(f"| Upscaler at 0.5 | Coverage | Longest gap | ({len(wire)} wire columns in the native image)")
    print("|---|---|---|")
    for upscaler in args.upscalers.split(","):
        png = args.out / f"{upscaler}.png"
        render(args.lab, {"mode": upscaler, "render_scale": 0.5}, png, args.frames)
        marks = kept(luminance_crop(png), wire)
        coverage = sum(marks) / len(marks)
        gap = longest = 0
        for mark in marks:
            gap = 0 if mark else gap + 1
            longest = max(longest, gap)
        print(f"| {upscaler} | {coverage:.0%} | {longest} |")
        if upscaler in args.require.split(",") and (coverage < 0.95 or longest > 3):
            print(f"upscale_wire: {upscaler} breaks the wire ({coverage:.0%} kept, a gap of {longest} columns)",
                  file=sys.stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
