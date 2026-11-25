#!/usr/bin/env python3
"""Writes the lab's Cornell box as glTF 2.0 (docs/specs/first-real-scene/spec.md, scene.AC5).

  python3 tools/assets/make_cornell_box.py apps/lab/assets/cornell_box

Our own asset, under the repository's license. Meters, +Y up, the open side facing +Z (glTF conventions).
- A 2 m room: white floor, ceiling and back wall, a red left wall and a green right wall.
- An emissive quad just below the ceiling: the area light (KHR_materials_emissive_strength).
- A point light (KHR_lights_punctual) above the boxes.
- A tall white box, and a short box whose material is the lab's GlowMaterial (extras.veach_material), which
  overrides only evalEmission.
- A cutout panel: a checkerboard base-color texture whose alpha punches holes (alphaMode MASK), so the golden
  covers texture cooking (BC7) and alpha testing in the path tracer's ray queries.
The output is deterministic: running it again gives identical files.
"""

from __future__ import annotations

import json
import math
import struct
import sys
import zlib
from pathlib import Path


def checker_png(size: int, cells: int) -> bytes:
    """An RGBA PNG: warm white cells with alpha 255 alternating with transparent ones."""
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # filter: none
        for x in range(size):
            solid = ((x * cells // size) + (y * cells // size)) % 2 == 0
            rows += bytes((235, 220, 190, 255)) if solid else bytes((0, 0, 0, 0))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + \
        chunk(b"IEND", b"")


def quad(corners, normal):
    """Two triangles, counter-clockwise seen from where `normal` points, with 0..1 UVs."""
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    vertices = [(c, normal, uv) for c, uv in zip(corners, uvs)]
    return vertices, [0, 1, 2, 0, 2, 3]


def box(size_x, size_y, size_z):
    """A box with its base on y = 0, centered on x and z, normals pointing out."""
    x, y, z = size_x / 2, size_y, size_z / 2
    faces = [
        ([(-x, 0, z), (x, 0, z), (x, y, z), (-x, y, z)], (0, 0, 1)),
        ([(x, 0, -z), (-x, 0, -z), (-x, y, -z), (x, y, -z)], (0, 0, -1)),
        ([(x, 0, z), (x, 0, -z), (x, y, -z), (x, y, z)], (1, 0, 0)),
        ([(-x, 0, -z), (-x, 0, z), (-x, y, z), (-x, y, -z)], (-1, 0, 0)),
        ([(-x, y, z), (x, y, z), (x, y, -z), (-x, y, -z)], (0, 1, 0)),
    ]
    return merge([quad(corners, n) for corners, n in faces])


def merge(parts):
    vertices, indices = [], []
    for v, i in parts:
        base = len(vertices)
        vertices += v
        indices += [base + k for k in i]
    return vertices, indices


class Builder:
    def __init__(self):
        self.bin = bytearray()
        self.gltf = {
            "asset": {"version": "2.0", "generator": "veach tools/assets/make_cornell_box.py"},
            "extensionsUsed": ["KHR_lights_punctual", "KHR_materials_emissive_strength"],
            "scene": 0, "scenes": [{"name": "cornell box", "nodes": []}],
            "nodes": [], "meshes": [], "materials": [], "accessors": [], "bufferViews": [],
            "buffers": [], "extensions": {"KHR_lights_punctual": {"lights": []}},
        }

    def view(self, data: bytes, target: int) -> int:
        while len(self.bin) % 4:
            self.bin.append(0)
        self.gltf["bufferViews"].append(
            {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data), "target": target})
        self.bin += data
        return len(self.gltf["bufferViews"]) - 1

    def accessor(self, values, kind: str) -> int:
        size = {"VEC3": 3, "VEC2": 2, "SCALAR": 1}[kind]
        if kind == "SCALAR":
            data = struct.pack(f"<{len(values)}I", *values)
            entry = {"componentType": 5125, "count": len(values), "type": kind,
                     "bufferView": self.view(data, 34963)}
        else:
            flat = [c for v in values for c in v]
            data = struct.pack(f"<{len(flat)}f", *flat)
            entry = {"componentType": 5126, "count": len(values), "type": kind,
                     "bufferView": self.view(data, 34962)}
            if kind == "VEC3":
                entry["min"] = [min(v[i] for v in values) for i in range(3)]
                entry["max"] = [max(v[i] for v in values) for i in range(3)]
        self.gltf["accessors"].append(entry)
        return len(self.gltf["accessors"]) - 1

    def material(self, name, color, emissive=(0, 0, 0), strength=1.0, kind=None, texture=None) -> int:
        m = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*color, 1.0], "metallicFactor": 0.0,
                                                    "roughnessFactor": 1.0}}
        if texture is not None:
            m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture}
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.5
            m["doubleSided"] = True
        if any(emissive):
            m["emissiveFactor"] = list(emissive)
            m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": strength}}
        if kind:
            m["extras"] = {"veach_material": kind}
        self.gltf["materials"].append(m)
        return len(self.gltf["materials"]) - 1

    def mesh(self, name, primitives) -> int:
        out = []
        for (vertices, indices), material in primitives:
            out.append({"attributes": {"POSITION": self.accessor([v[0] for v in vertices], "VEC3"),
                                       "NORMAL": self.accessor([v[1] for v in vertices], "VEC3"),
                                       "TEXCOORD_0": self.accessor([v[2] for v in vertices], "VEC2")},
                        "indices": self.accessor(indices, "SCALAR"), "material": material})
        self.gltf["meshes"].append({"name": name, "primitives": out})
        return len(self.gltf["meshes"]) - 1

    def node(self, name, mesh=None, translation=(0, 0, 0), yaw_degrees=0.0, light=None):
        n = {"name": name, "translation": list(translation)}
        if yaw_degrees:
            half = math.radians(yaw_degrees) / 2
            n["rotation"] = [0.0, round(math.sin(half), 7), 0.0, round(math.cos(half), 7)]
        if mesh is not None:
            n["mesh"] = mesh
        if light is not None:
            n["extensions"] = {"KHR_lights_punctual": {"light": light}}
        self.gltf["nodes"].append(n)
        self.gltf["scenes"][0]["nodes"].append(len(self.gltf["nodes"]) - 1)


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "apps/lab/assets/cornell_box")
    out.mkdir(parents=True, exist_ok=True)
    b = Builder()
    white = b.material("white", (0.73, 0.73, 0.73))
    red = b.material("red", (0.63, 0.065, 0.05))
    green = b.material("green", (0.14, 0.45, 0.091))
    light = b.material("light", (0.78, 0.78, 0.78), emissive=(1.0, 0.85, 0.6), strength=12.0)
    glow = b.material("glow", (0.73, 0.73, 0.73), emissive=(1.0, 0.45, 0.1), strength=3.0, kind="GlowMaterial")
    b.gltf["images"] = [{"uri": "checker.png"}]
    b.gltf["textures"] = [{"source": 0}]
    cutout = b.material("cutout", (1.0, 1.0, 1.0), texture=0)

    s = 1.0  # half the room's size
    walls = [
        (quad([(-s, 0, s), (s, 0, s), (s, 0, -s), (-s, 0, -s)], (0, 1, 0)), white),          # floor
        (quad([(-s, 2, -s), (s, 2, -s), (s, 2, s), (-s, 2, s)], (0, -1, 0)), white),         # ceiling
        (quad([(-s, 0, -s), (s, 0, -s), (s, 2, -s), (-s, 2, -s)], (0, 0, 1)), white),        # back
        (quad([(-s, 0, s), (-s, 0, -s), (-s, 2, -s), (-s, 2, s)], (1, 0, 0)), red),          # left
        (quad([(s, 0, -s), (s, 0, s), (s, 2, s), (s, 2, -s)], (-1, 0, 0)), green),           # right
    ]
    white_walls = merge([w[0] for w in walls if w[1] == white])
    b.node("walls", b.mesh("walls", [(white_walls, white)] + [w for w in walls if w[1] != white]))
    l = 0.25
    b.node("area light", b.mesh("area light", [(quad([(-l, 1.99, -l), (l, 1.99, -l), (l, 1.99, l), (-l, 1.99, l)],
                                                     (0, -1, 0)), light)]))
    b.node("tall box", b.mesh("tall box", [(box(0.6, 1.2, 0.6), white)]), (-0.35, 0.0, -0.3), 18.0)
    b.node("glow block", b.mesh("glow block", [(box(0.6, 0.6, 0.6), glow)]), (0.35, 0.0, 0.3), -18.0)
    p = 0.35  # a panel near the back wall, between the boxes
    b.node("cutout panel", b.mesh("cutout panel", [(quad([(-p, 0, 0), (p, 0, 0), (p, 2 * p, 0), (-p, 2 * p, 0)],
                                                         (0, 0, 1)), cutout)]), (0.15, 0.95, -0.75))
    b.gltf["extensions"]["KHR_lights_punctual"]["lights"].append(
        {"name": "fill", "type": "point", "color": [1.0, 0.95, 0.9], "intensity": 0.6})
    b.node("fill light", translation=(0.0, 1.6, 0.6), light=0)

    b.gltf["buffers"].append({"uri": "cornell_box.bin", "byteLength": len(b.bin)})
    (out / "cornell_box.bin").write_bytes(bytes(b.bin))
    (out / "cornell_box.gltf").write_text(json.dumps(b.gltf, indent=1) + "\n")
    (out / "checker.png").write_bytes(checker_png(64, 8))
    print(f"wrote {out}/cornell_box.gltf and cornell_box.bin ({len(b.bin)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
