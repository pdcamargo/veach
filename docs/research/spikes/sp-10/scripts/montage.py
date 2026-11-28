#!/usr/bin/env python3
"""Spike SP-10 (VCH-086): tile PNG screenshots into one strip or grid, downscaled (pure Python, no Pillow).

  python3 montage.py OUT.png --columns N --scale K IN1.png IN2.png ...

Also prints each image's mean linear luminance, for the seam tables.
"""
import argparse
import struct
import sys
import zlib


def read_png(path: str) -> tuple[int, int, bytearray]:
    """An 8-bit RGB or RGBA PNG as packed RGB rows, top row first."""
    data = open(path, "rb").read()
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
    out = bytearray()
    previous, i = bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if kind != 0:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                b = previous[x]
                if kind == 1:
                    line[x] = (line[x] + a) & 255
                elif kind == 2:
                    line[x] = (line[x] + b) & 255
                elif kind == 3:
                    line[x] = (line[x] + (a + b) // 2) & 255
                elif kind == 4:
                    c = previous[x - channels] if x >= channels else 0
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if channels == 3:
            out += line
        else:
            for x in range(0, stride, 4):
                out += line[x:x + 3]
        previous = line
    return width, height, out


def write_png(path: str, width: int, height: int, rgb: bytearray) -> None:
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgb[y * width * 3:(y + 1) * width * 3]

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def to_linear(c: int) -> float:
    v = c / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("inputs", nargs="+")
    parser.add_argument("--columns", type=int, default=4)
    parser.add_argument("--scale", type=int, default=2)
    args = parser.parse_args()
    table = [to_linear(i) for i in range(256)]
    images = []
    for path in args.inputs:
        w, h, rgb = read_png(path)
        n = w * h
        mean = sum(0.2126 * table[rgb[3 * i]] + 0.7152 * table[rgb[3 * i + 1]] + 0.0722 * table[rgb[3 * i + 2]]
                   for i in range(0, n, 7)) / len(range(0, n, 7))
        print(f"{path}: mean linear luminance (display-referred) {mean:.4f}")
        k = args.scale
        sw, sh = w // k, h // k
        small = bytearray(sw * sh * 3)
        for y in range(sh):
            for x in range(sw):
                for ch in range(3):
                    total = 0
                    for dy in range(k):
                        row = (y * k + dy) * w
                        for dx in range(k):
                            total += rgb[3 * (row + x * k + dx) + ch]
                    small[3 * (y * sw + x) + ch] = total // (k * k)
        images.append((sw, sh, small))
    cw = max(i[0] for i in images)
    chh = max(i[1] for i in images)
    gap = 4
    cols = min(args.columns, len(images))
    rows = (len(images) + cols - 1) // cols
    width, height = cols * cw + (cols - 1) * gap, rows * chh + (rows - 1) * gap
    canvas = bytearray([255]) * (width * height * 3)
    for index, (sw, sh, small) in enumerate(images):
        ox, oy = (index % cols) * (cw + gap), (index // cols) * (chh + gap)
        for y in range(sh):
            start = 3 * ((oy + y) * width + ox)
            canvas[start:start + 3 * sw] = small[3 * y * sw:3 * (y + 1) * sw]
    write_png(args.out, width, height, canvas)
    return 0


if __name__ == "__main__":
    sys.exit(main())
