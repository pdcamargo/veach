#!/usr/bin/env python3
"""Spike SP-14 (VCH-090): every screenshot and number of docs/research/spikes/sp-14-exposure-depth.md.

  python3 docs/research/spikes/sp-14/scripts/shots.py --lab build/release/apps/lab/veach-lab \
      --out docs/research/spikes/sp-14 [--only depth|cap|exposure|noclamp|scale]

Each shot is one headless run of the lab with VEACH_SP14_* set (apps/lab/src/sp14_spike.hpp), from apps/lab. The
depth shots run with VEACH_SP14_CHECK=1, so the exposure pass counts non-finite pixels: a run that logs an error
(non-zero exit) stops the script, except the run that removes the fp16 clamp on purpose.
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngio  # noqa: E402

LAB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../../apps/lab")


def run(lab: str, env: dict[str, str], frames: int, screenshot: str | None, allow_errors: bool = False) -> list[dict]:
    """Runs the lab; returns its JSON log records."""
    run_env = {k: v for k, v in os.environ.items() if not k.startswith("VEACH_SP")}
    run_env.update(env)
    args = [lab, "--rhi", "metal", "--headless", "--frames", str(frames), "--log-format", "json"]
    if screenshot:
        os.makedirs(os.path.dirname(os.path.abspath(screenshot)), exist_ok=True)
        args += ["--screenshot", os.path.abspath(screenshot)]
    result = subprocess.run(args, cwd=LAB_DIR, env=run_env, capture_output=True, text=True)
    records = []
    for line in (result.stdout + result.stderr).splitlines():
        if line.startswith("{"):
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    if result.returncode != 0 and not allow_errors:
        errors = [r["msg"] for r in records if r.get("level") == "error"]
        sys.exit(f"lab failed ({result.returncode}) with {env}: {errors or result.stderr[-2000:]}")
    return records


def exposure_rows(records: list[dict]) -> list[dict]:
    rows = []
    for r in records:
        if r.get("cat") != "exposure":
            continue
        words = r["msg"].split()
        row = {}
        i = 0
        while i < len(words):
            key = words[i]
            if key == "band":
                row["band_low"], row["band_high"] = float(words[i + 1]), float(words[i + 2])
                i += 3
                continue
            row[key] = float(words[i + 1])
            i += 2
        rows.append(row)
    rows.sort(key=lambda r: r["frame"])
    return rows


# ---- depth ------------------------------------------------------------------------------------------------------

def depth(lab: str, out: str) -> None:
    shots = []
    for scale in ["1", "0.01"]:
        shots.append((f"depth/wide_s{scale}", {"VEACH_SP14_SCALE": scale, "VEACH_SP14_SIZE": "1920x1080"}))
        shots.append((f"depth/moon_s{scale}", {"VEACH_SP14_SCALE": scale, "VEACH_SP14_VIEW": "moon"}))
        shots.append((f"depth/limb_s{scale}", {"VEACH_SP14_SCALE": scale, "VEACH_SP14_VIEW": "limb"}))
    # Camera-relative against float32 world positions, the camera far from the world origin (S5): 1e8 m and 1 AU.
    for name, origin in [("1e8", "1e8"), ("1au", "1.496e11")]:
        shots.append((f"depth/relative_{name}", {"VEACH_SP14_ORIGIN": origin, "VEACH_SP14_SIZE": "1920x1080"}))
        shots.append((f"depth/float_{name}", {"VEACH_SP14_ORIGIN": origin, "VEACH_SP14_FLOAT": "1",
                                              "VEACH_SP14_SIZE": "1920x1080"}))
    for name, env in shots:
        env = {"VEACH_SP14": "depth", "VEACH_SP14_CHECK": "1", **env}
        records = run(lab, env, 3, os.path.join(out, name + ".png"))
        rows = exposure_rows(records)
        print(name, "non-finite", int(rows[-1]["nonfinite"]) if rows else "?", file=sys.stderr)
    # The ship moving 1 cm per frame, 1e6 m from the origin (float32 steps there are 6 cm, so the cockpit survives
    # but jumps): frames 30 to 36, camera-relative and float32, and the FLIP between consecutive frames.
    imgdiff = os.path.join(os.path.dirname(lab), "../../tools/veach-cli/veach")
    for mode, extra in [("relative", {}), ("float", {"VEACH_SP14_FLOAT": "1"})]:
        flips = []
        for frames in range(30, 37):
            env = {"VEACH_SP14": "depth", "VEACH_SP14_ORIGIN": "1e6", "VEACH_SP14_MOVE": "0.01", **extra}
            path = os.path.join(out, f"depth/move/{mode}_f{frames}.png")
            run(lab, env, frames, path)
            if frames > 30:
                result = subprocess.run([imgdiff, "imgdiff", os.path.join(out, f"depth/move/{mode}_f{frames - 1}.png"),
                                         path], capture_output=True, text=True)
                text = result.stdout + result.stderr
                flips.append(float(text.split("mean FLIP ")[1].split()[0]))
        print(f"moving 1 cm per frame 1e6 m from the origin, {mode}: mean FLIP between consecutive frames "
              + ", ".join(f"{f:.4f}" for f in flips))


# ---- the depth ladder: a red cap delta meters above the moon ------------------------------------------------------

def red_mask(path: str) -> tuple[int, int, list[bool]]:
    w, h, px = pngio.read(path)
    mask = []
    for i in range(w * h):
        r, g, b = px[4 * i], px[4 * i + 1], px[4 * i + 2]
        mask.append(r > 30 and r > 2 * g and r > 2 * b)
    return w, h, mask


def erode(w: int, h: int, mask: list[bool]) -> list[bool]:
    out = [False] * (w * h)
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            i = y * w + x
            out[i] = mask[i] and mask[i - 1] and mask[i + 1] and mask[i - w] and mask[i + w]
    return out


def cap(lab: str, out: str) -> None:
    """A red shell delta meters above the moon, with the moon's own tessellation, drawn after it. The first (highest)
    shell is the reference: inside its lit red disk (eroded by a pixel), any pixel that is not red is the moon
    winning the depth test, i.e. depth fighting."""
    ladders = {"1": ["3000", "1000", "300", "100", "30", "10", "3", "1"],
               "0.01": ["30", "10", "3", "1", "0.3", "0.1", "0.03", "0.01"]}
    print("| s | moon at | shell height (m) | relative to the distance | fighting pixels |")
    print("|---|---|---|---|---|")
    for scale, deltas in ladders.items():
        reference = None
        for delta in deltas:
            path = os.path.join(out, f"cap/s{scale}_{delta}m.png")
            run(lab, {"VEACH_SP14": "depth", "VEACH_SP14_VIEW": "cap", "VEACH_SP14_SCALE": scale,
                      "VEACH_SP14_CAP": delta, "VEACH_SP14_CHECK": "1"}, 3, path)
            w, h, mask = red_mask(path)
            if reference is None:
                reference = erode(w, h, mask)
            inside = sum(1 for r in reference if r)
            lost = sum(1 for m, r in zip(mask, reference) if r and not m)
            distance = 3.84e8 * float(scale)
            print(f"| {scale} | {distance:.3g} m | {delta} | {float(delta) / distance:.1e} | "
                  f"{100.0 * lost / inside:.2f}% of {inside} |")


# ---- exposure ----------------------------------------------------------------------------------------------------

FRAMES = 2250
PHASES = [(0, "A ground"), (180, "B cave (cut, walk in)"), (360, "C deep cave"), (660, "D turn to the mouth"),
          (960, "E orbit (cut)"), (1260, "F the sun"), (1440, "G deep space"), (1950, "H ground (cut)")]


def svg_curve(rows: list[dict], path: str, title: str, extra: list[tuple[str, list[dict]]] = ()) -> None:
    w, h, left, right, top, bottom = 960, 360, 56, 16, 28, 40
    lo, hi = -6.0, 18.0
    n = max(r["frame"] for r in rows)

    def x(f: float) -> float:
        return left + (w - left - right) * (f - 1) / (n - 1)

    def y(ev: float) -> float:
        return top + (h - top - bottom) * (hi - ev) / (hi - lo)

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
             'font-family="system-ui, sans-serif" font-size="11">',
             f'<rect width="{w}" height="{h}" fill="#ffffff"/>',
             f'<text x="{left}" y="16" font-size="13" fill="#222">{title}</text>']
    for ev in range(int(lo), int(hi) + 1, 2):
        parts.append(f'<line x1="{left}" x2="{w - right}" y1="{y(ev):.1f}" y2="{y(ev):.1f}" stroke="#e5e5e5"/>')
        parts.append(f'<text x="{left - 6}" y="{y(ev) + 4:.1f}" text-anchor="end" fill="#666">{ev}</text>')
    for frame, label in PHASES:
        parts.append(f'<line x1="{x(frame + 1):.1f}" x2="{x(frame + 1):.1f}" y1="{top}" y2="{h - bottom}" '
                     'stroke="#bbb" stroke-dasharray="3 3"/>')
        parts.append(f'<text x="{x(frame + 1) + 3:.1f}" y="{h - bottom + 14}" fill="#444">{label.split()[0]}</text>')
    parts.append(f'<text x="{w / 2}" y="{h - 6}" text-anchor="middle" fill="#444">frame (60 per second); '
                 'A ground, B cave, C deep cave, D the mouth, E orbit, F the sun, G deep space, H ground</text>')
    parts.append(f'<text x="14" y="{(top + h - bottom) / 2}" fill="#444" transform="rotate(-90 14 '
                 f'{(top + h - bottom) / 2})" text-anchor="middle">EV100</text>')

    def line(data: list[dict], key: str, color: str, width: float, dash: str = "") -> None:
        points = " ".join(f"{x(r['frame']):.1f},{y(min(max(r[key], lo), hi)):.1f}" for r in data)
        parts.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="{width}" '
                     f'{f"stroke-dasharray={chr(34)}{dash}{chr(34)}" if dash else ""}/>')

    line(rows, "target", "#e08a00", 1.2, "4 3")
    line(rows, "ev100", "#1f5fbf", 2.0)
    legend = [("#1f5fbf", "adapted EV100"), ("#e08a00", "metered target (clamped to -4..18)")]
    for label, data in extra:
        line(data, "ev100", "#7a3db8", 1.4, "2 2")
        legend.append(("#7a3db8", label))
    for i, (color, label) in enumerate(legend):
        parts.append(f'<line x1="{w - 300}" x2="{w - 276}" y1="{top + 10 + 16 * i}" y2="{top + 10 + 16 * i}" '
                     f'stroke="{color}" stroke-width="2"/>')
        parts.append(f'<text x="{w - 270}" y="{top + 14 + 16 * i}" fill="#222">{label}</text>')
    parts.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(parts) + "\n")


def clipping(path: str) -> tuple[float, float, float]:
    """Fractions of pixels shown white (every channel >= 250), black (every channel <= 2), and their mean luma."""
    w, h, px = pngio.read(path)
    white = black = 0
    total = 0.0
    for i in range(w * h):
        r, g, b = px[4 * i], px[4 * i + 1], px[4 * i + 2]
        if min(r, g, b) >= 250:
            white += 1
        if max(r, g, b) <= 2:
            black += 1
        total += 0.2126 * r + 0.7152 * g + 0.0722 * b
    return white / (w * h), black / (w * h), total / (w * h)


def strip(paths: list[str], out_path: str, columns: int = 4, factor: int = 4) -> None:
    images = [pngio.read(p) for p in paths]
    w, h = images[0][0] // factor, images[0][1] // factor
    rows = (len(images) + columns - 1) // columns
    gap = 4
    W, H = columns * w + (columns - 1) * gap, rows * h + (rows - 1) * gap
    out = bytearray([96, 96, 96, 255] * (W * H))  # grey gaps, so white frames stand out
    for k, (iw, ih, px) in enumerate(images):
        ox, oy = (k % columns) * (w + gap), (k // columns) * (h + gap)
        for y in range(h):
            for x in range(w):
                # A box filter over the factor x factor block.
                acc = [0, 0, 0]
                for dy in range(factor):
                    base = ((y * factor + dy) * iw + x * factor) * 4
                    for dx in range(factor):
                        acc[0] += px[base + 4 * dx]
                        acc[1] += px[base + 4 * dx + 1]
                        acc[2] += px[base + 4 * dx + 2]
                o = ((oy + y) * W + ox + x) * 4
                n = factor * factor
                out[o:o + 4] = bytes([acc[0] // n, acc[1] // n, acc[2] // n, 255])
    pngio.write(out_path, W, H, out)


def shots_in_order() -> dict[str, int]:
    """The exposure shots and their frames, from the spike's table (exposure_shots in sp14_spike.cpp), in order."""
    source = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "../../../../../apps/lab/src/sp14_spike.cpp")).read()
    found = [(int(frame), name) for frame, name in re.findall(r'\{(\d+), "([a-z0-9_.]+)"\}', source)]
    return {name: frame for frame, name in sorted(found)}


def exposure(lab: str, out: str) -> None:
    folder = os.path.join(out, "exposure")
    records = run(lab, {"VEACH_SP14": "exposure", "VEACH_SP14_OUT": os.path.abspath(folder)}, FRAMES, None)
    rows = exposure_rows(records)
    with open(os.path.join(folder, "ev.csv"), "w") as f:
        keys = list(rows[0].keys())
        f.write(",".join(keys) + "\n")
        for r in rows:
            f.write(",".join(f"{r[k]:g}" for k in keys) + "\n")
    print(f"exposure: {len(rows)} frames, non-finite {int(sum(r['nonfinite'] for r in rows))}, "
          f"saturated (clamped) pixel-frames {int(sum(r['saturated'] for r in rows))}", file=sys.stderr)
    shot_frames = shots_in_order()
    names = [name + ".png" for name in shot_frames]
    from_frame = {int(r["frame"]): r for r in rows}
    print("| shot | EV100 | target | white | black | white (HDR) | black (HDR) | mean luma |")
    print("|---|---|---|---|---|---|---|---|")
    for name in names:
        white, black, luma = clipping(os.path.join(folder, name))
        r = from_frame.get(shot_frames.get(name[:-4], -1), {})
        print(f"| {name[:-4]} | {r.get('ev100', float('nan')):.2f} | {r.get('target', float('nan')):.2f} | "
              f"{100 * white:.2f}% | {100 * black:.2f}% | {100 * r.get('white', 0):.2f}% | "
              f"{100 * r.get('black', 0):.2f}% | {luma:.0f} |")
    strip([os.path.join(folder, n) for n in names], os.path.join(folder, "strip.png"))
    svg_curve(rows, os.path.join(folder, "ev.svg"), "SP-14: auto-exposure along the timeline (s = 1)")


def noclamp(lab: str, out: str) -> None:
    """Without the explicit fp16 clamp: non-finite pixels, and pixels at the fp16 maximum, per phase."""
    for label, env in [("clamp", {}), ("no clamp", {"VEACH_SP14_NO_CLAMP": "1"})]:
        records = run(lab, {"VEACH_SP14": "exposure", **env}, FRAMES, None, allow_errors=True)
        rows = exposure_rows(records)
        phases: dict[str, list[int]] = {}
        for r in rows:
            phase = max(p for p in PHASES if p[0] < r["frame"])[1]
            entry = phases.setdefault(phase, [0, 0, 0])
            entry[0] += int(r["nonfinite"])
            entry[1] += int(r["saturated"])
            entry[2] += 1 if r["saturated"] > 0 else 0
        print(f"{label}: non-finite pixels {sum(e[0] for e in phases.values())}; per phase (non-finite, saturated "
              f"pixel-frames, frames with any): {phases}")


def pull(lab: str, out: str) -> None:
    """Looking out of the cave (phase D) with the highlight's pull on the metered exposure at 0, 2, 4 (default) and
    unlimited stops: the window blown against the interior crushed."""
    print("| highlight pull (stops) | EV100 | white | black | mean luma |")
    print("|---|---|---|---|---|")
    for stops in ["0", "2", "4", "99"]:
        folder = os.path.join(out, f"exposure/pull_{stops}")
        records = run(lab, {"VEACH_SP14": "exposure", "VEACH_SP14_FROM": "660", "VEACH_SP14_PULL": stops,
                            "VEACH_SP14_OUT": folder}, 300, None)
        rows = exposure_rows(records)
        white, black, luma = clipping(os.path.join(folder, "d_mouth.png"))
        ev = next(r["ev100"] for r in rows if r["frame"] == 950 - 660)
        print(f"| {stops} | {ev:.2f} | {100 * white:.2f}% | {100 * black:.2f}% | {luma:.0f} |")
        for name in os.listdir(folder):
            if name != "d_mouth.png":
                os.remove(os.path.join(folder, name))


def scale(lab: str, out: str) -> None:
    """The same timeline at s = 0.01. Only the ground and cave phases are the same scene (the orbit stays 400 km up,
    which is 6 radii at s = 0.01), so only they are compared."""
    folder = os.path.join(out, "exposure")
    rows1 = []
    with open(os.path.join(folder, "ev.csv")) as f:
        keys = f.readline().strip().split(",")
        for line in f:
            rows1.append(dict(zip(keys, map(float, line.strip().split(",")))))
    records = run(lab, {"VEACH_SP14": "exposure", "VEACH_SP14_SCALE": "0.01"}, FRAMES, None)
    rows = exposure_rows(records)
    ground = [(a, b) for a, b in zip(rows, rows1) if a["frame"] <= 960 or a["frame"] > 1950]
    diff = max(abs(a["ev100"] - b["ev100"]) for a, b in ground)
    print(f"s = 0.01 against s = 1, ground and cave phases: largest EV100 difference {diff:.3f}")
    svg_curve(rows1, os.path.join(folder, "ev_scales.svg"), "SP-14: auto-exposure at s = 1 and s = 0.01",
              [("adapted EV100 at s = 0.01 (orbit: 400 km is 6 radii)", rows)])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    lab = os.path.abspath(args.lab)
    out = os.path.abspath(args.out)
    for name, fn in [("depth", depth), ("cap", cap), ("exposure", exposure), ("pull", pull), ("noclamp", noclamp),
                     ("scale", scale)]:
        if not args.only or args.only == name:
            fn(lab, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
