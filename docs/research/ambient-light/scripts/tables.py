#!/usr/bin/env python3
"""Markdown tables from sweep.py's timings.json (docs/research/ambient-light/README.md).

  python3 docs/research/ambient-light/scripts/tables.py [docs/research/ambient-light/timings.json]
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AO = ("ao: view depth", "ao: depth mip", "ao: trace", "ao: denoise", "ao: upsample")


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "../timings.json")
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    header = ("| Scene | Size | Build | Total | Shade | Sky irradiance | AO: depth | AO: trace | AO: denoise | "
              "AO: upsample | AO total | Visibility | Display |")
    print(header)
    print("|" + "---|" * (header.count("|") - 1))
    for name in sorted(data, key=lambda n: (n.split("|")[0], n.split("|")[1])):
        scene, size = name.split("|")
        for label in ("before", "after"):
            r = data[name].get(label)
            if r is None:
                continue
            p = r["passes"]

            def total(prefix: str) -> float:
                return sum(v for k, v in p.items() if k.startswith(prefix))

            depth = total("ao: view depth") + total("ao: depth mip")
            ao = sum(total(a) for a in AO)
            print(f"| {scene} | {size} | {label} | {r['total_ms']:.2f} | {p.get('shade', 0.0):.2f} | "
                  f"{p.get('sky irradiance', 0.0):.2f} | {depth:.2f} | {total('ao: trace'):.2f} | "
                  f"{total('ao: denoise'):.2f} | {total('ao: upsample'):.2f} | {ao:.2f} | "
                  f"{total('visibility'):.2f} | {p.get('display', 0.0):.2f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
