#!/usr/bin/env python3
"""Writes the lab's thin-triangles scene as glTF 2.0 (VCH-108, docs/research/notes/vch-108-pinholes.md).

  python3 tools/assets/make_thin_triangles.py apps/lab/assets/thin_triangles

Our own asset, under the repository's license. Meters, +Y up (glTF conventions). What SP-08's Transvoxel chunks showed
along their seams, in a few triangles: triangles standing upright in a smooth surface, in a vertical plane, with the
surface's vertex normals (+Y) instead of their own. Transvoxel leaves such triangles in a chunk's face plane.

- Primitive 0: a 4 m x 5 m ground grid, normals up.
- Primitive 1, sharing primitive 0's vertex arrays as SP-08's export did (one POSITION and NORMAL accessor for both
  primitives, two index ranges):
  - two 25 cm fins on z = 0, normals up: the left one wound to face the camera (+Z), the right one wound to face away;
  - two rows of 1.2 cm slivers (the width SP-08 measured), on z = 0.8 and z = -2, each half facing the camera and half
    facing away: about a pixel tall at 256 x 256.
Both primitives have the same light gray material (two materials, as SP-08's export had): with the ground's normals and
the ground's material, the fins and slivers must not show in an unshadowed image. Where they show, they are shaded
wrong, and how much of a sliver the rasterizer covers (which may differ a little between GPUs) does not matter.

The output is deterministic: running it again gives identical files.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

UP = (0.0, 1.0, 0.0)


def ground(vertices, indices, x0, x1, z0, z1, nx, nz):
    """A grid on y = 0, counter-clockwise seen from above."""
    base = len(vertices)
    for j in range(nz + 1):
        for i in range(nx + 1):
            vertices.append((x0 + (x1 - x0) * i / nx, 0.0, z0 + (z1 - z0) * j / nz))
    for j in range(nz):
        for i in range(nx):
            a = base + j * (nx + 1) + i
            b, c, d = a + 1, a + nx + 2, a + nx + 1
            indices += [a, d, c, a, c, b]  # z grows toward +Z: (a, d, c) is counter-clockwise from +Y


def fin(vertices, indices, x0, x1, z, height, quads, facing_camera):
    """An upright strip on the plane z, from y = 0 to `height`, split into `quads` quads; counter-clockwise seen from +Z
    when it faces the camera, from -Z otherwise."""
    base = len(vertices)
    for i in range(quads + 1):
        x = x0 + (x1 - x0) * i / quads
        vertices.append((x, 0.0, z))
        vertices.append((x, height, z))
    for i in range(quads):
        a, b, c, d = base + 2 * i, base + 2 * i + 2, base + 2 * i + 3, base + 2 * i + 1  # counter-clockwise from +Z
        indices += [a, b, c, a, c, d] if facing_camera else [a, c, b, a, d, c]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    vertices: list[tuple[float, float, float]] = []
    ground_indices: list[int] = []
    fin_indices: list[int] = []
    ground(vertices, ground_indices, -2.0, 2.0, -3.0, 2.0, 8, 10)
    fin(vertices, fin_indices, -1.0, -0.1, 0.0, 0.25, 4, True)
    fin(vertices, fin_indices, 0.1, 1.0, 0.0, 0.25, 4, False)
    for z in (0.8, -2.0):
        fin(vertices, fin_indices, -1.2, -0.05, z, 0.012, 23, True)
        fin(vertices, fin_indices, 0.05, 1.2, z, 0.012, 23, False)

    binary = bytearray()
    views, accessors = [], []

    def accessor(data: bytes, target: int, entry: dict) -> int:
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data), "target": target})
        binary.extend(data)
        entry["bufferView"] = len(views) - 1
        accessors.append(entry)
        return len(accessors) - 1

    lo = [min(v[k] for v in vertices) for k in range(3)]
    hi = [max(v[k] for v in vertices) for k in range(3)]
    position = accessor(b"".join(struct.pack("<3f", *v) for v in vertices), 34962,
                        {"componentType": 5126, "count": len(vertices), "type": "VEC3", "min": lo, "max": hi})
    normal = accessor(b"".join(struct.pack("<3f", *UP) for _ in vertices), 34962,
                      {"componentType": 5126, "count": len(vertices), "type": "VEC3"})
    primitives = []
    for material, indices in ((0, ground_indices), (1, fin_indices)):
        ia = accessor(struct.pack(f"<{len(indices)}I", *indices), 34963,
                      {"componentType": 5125, "count": len(indices), "type": "SCALAR"})
        primitives.append({"attributes": {"POSITION": position, "NORMAL": normal}, "indices": ia,
                           "material": material})

    def material(name, color):
        return {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*color, 1.0], "metallicFactor": 0.0,
                                                       "roughnessFactor": 0.9}}

    gltf = {
        "asset": {"version": "2.0", "generator": "veach tools/assets/make_thin_triangles.py"},
        "scene": 0,
        "scenes": [{"name": "thin triangles", "nodes": [0]}],
        "nodes": [{"name": "ground and fins", "mesh": 0}],
        "meshes": [{"name": "ground and fins", "primitives": primitives}],
        "materials": [material("ground", (0.5, 0.5, 0.5)), material("fins", (0.5, 0.5, 0.5))],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"uri": "thin_triangles.bin", "byteLength": len(binary)}],
    }
    (out / "thin_triangles.bin").write_bytes(bytes(binary))
    (out / "thin_triangles.gltf").write_text(json.dumps(gltf, indent=1) + "\n")
    # VCH-110: the same buffer with the ground's primitive alone (no thin triangles, one primitive), for the DX12
    # diagnostic that splits the scene's hypotheses.
    ground_only = json.loads(json.dumps(gltf))
    ground_only["scenes"][0]["name"] = "thin triangles, ground only"
    ground_only["nodes"][0]["name"] = ground_only["meshes"][0]["name"] = "ground"
    ground_only["meshes"][0]["primitives"] = ground_only["meshes"][0]["primitives"][:1]
    (out / "thin_triangles_ground.gltf").write_text(json.dumps(ground_only, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
