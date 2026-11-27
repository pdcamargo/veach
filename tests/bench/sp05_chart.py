#!/usr/bin/env python3
"""Spike SP-05 (VCH-081), throwaway: draws the histograms written by veach-bench-sp05-systems --out as one SVG.

    sp05_chart.py <csv dir, bias 0.5> <csv dir, bias 0> <out.svg>

Standard library only. Bars are percentages of each histogram's total; the last two panels compare the two bias runs.
"""
import csv
import pathlib
import sys
from xml.sax.saxutils import escape

PANELS = [
    ("star_class", "Star class (share of systems)", None),
    ("planets_per_system", "Planets per system", None),
    ("planet_mass", "Planet mass (Earth masses)", None),
    ("planet_radius", "Planet radius (Earth radii)", None),
    ("adjacent_period_ratio", "Adjacent period ratio P_out / P_in", None),
    ("adjacent_mutual_hill_spacing", "Adjacent spacing (mutual Hill radii)", None),
    ("planet_equilibrium_temperature", "Planet equilibrium temperature (K)", None),
    ("hz_planets_per_system", "Planets in the habitable zone, per system", "compare"),
    ("potentially_habitable_per_system", "Potentially habitable bodies, per system", "compare"),
]

W, H = 380, 250  # one panel
COLS = 3
PAD_L, PAD_R, PAD_T, PAD_B = 44, 12, 44, 62


def read(path):
    with open(path, newline="") as f:
        return [(r["bin"], float(r["fraction"])) for r in csv.DictReader(f)]


def short(label):
    return label.replace(" - ", "–").replace(">= ", "≥").replace("< ", "<")


def bar(x, y0, w, h, cls):
    """A bar with 4 px rounded data end, anchored square on the baseline."""
    r = min(4.0, w / 2, h)
    if h <= 0:
        return ""
    top = y0 - h
    return (f'<path class="{cls}" d="M{x:.1f},{y0:.1f} V{top + r:.1f} Q{x:.1f},{top:.1f} {x + r:.1f},{top:.1f} '
            f'H{x + w - r:.1f} Q{x + w:.1f},{top:.1f} {x + w:.1f},{top + r:.1f} V{y0:.1f} Z"/>')


def panel(ox, oy, title, series, labels):
    out = [f'<g transform="translate({ox},{oy})">', f'<text class="title" x="{PAD_L}" y="20">{escape(title)}</text>']
    pw, ph = W - PAD_L - PAD_R, H - PAD_T - PAD_B
    y0 = PAD_T + ph
    top = max(max(v for _, v in s) for s in series)
    step = 0.05 if top <= 0.3 else 0.1 if top <= 0.6 else 0.2
    ymax = step * (int(top / step) + 1)
    k = 0.0
    while k <= ymax + 1e-9:
        y = y0 - ph * k / ymax
        out.append(f'<line class="grid" x1="{PAD_L}" x2="{PAD_L + pw}" y1="{y:.1f}" y2="{y:.1f}"/>')
        out.append(f'<text class="tick" x="{PAD_L - 6}" y="{y + 4:.1f}" text-anchor="end">{k * 100:.0f}%</text>')
        k += step
    n = len(labels)
    slot = pw / n
    group = slot - 2  # 2 px surface gap between neighbors
    bw = (group - 2 * (len(series) - 1)) / len(series)
    for i, lab in enumerate(labels):
        for j, s in enumerate(series):
            v = s[i][1]
            x = PAD_L + i * slot + 1 + j * (bw + 2)
            out.append(bar(x, y0, bw, ph * v / ymax, f"s{j + 1}"))
        cx = PAD_L + i * slot + slot / 2
        out.append(f'<text class="tick" transform="translate({cx:.1f},{y0 + 10}) rotate(-45)" text-anchor="end">'
                   f'{escape(short(lab))}</text>')
    if len(series) == 1:  # one direct label, on the tallest bar
        for i in sorted(range(n), key=lambda i: -series[0][i][1])[:1]:
            v = series[0][i][1]
            cx = PAD_L + i * slot + slot / 2
            out.append(f'<text class="value" x="{cx:.1f}" y="{y0 - ph * v / ymax - 4:.1f}" text-anchor="middle">'
                       f'{v * 100:.0f}%</text>')
    out.append(f'<line class="axis" x1="{PAD_L}" x2="{PAD_L + pw}" y1="{y0}" y2="{y0}"/>')
    out.append("</g>")
    return out


def main():
    a, b, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    rows = (len(PANELS) + COLS - 1) // COLS
    width, height = COLS * W, rows * H + 70
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
           f'font-family="-apple-system, Helvetica, Arial, sans-serif">',
           "<style>",
           ":root{--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;"
           "--s1:#2a78d6;--s2:#eb6834}",
           "@media (prefers-color-scheme: dark){:root{--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--grid:#2c2c2a;"
           "--axis:#383835;--s1:#3987e5;--s2:#d95926}}",
           ".bg{fill:var(--surface)} .title{fill:var(--ink);font-size:13px;font-weight:600}"
           " .tick{fill:var(--muted);font-size:10px} .value{fill:var(--ink2);font-size:10px}"
           " .grid{stroke:var(--grid);stroke-width:1} .axis{stroke:var(--axis);stroke-width:1}"
           " .s1{fill:var(--s1)} .s2{fill:var(--s2)} .legend{fill:var(--ink2);font-size:12px}",
           "</style>",
           f'<rect class="bg" width="{width}" height="{height}"/>',
           '<text class="title" x="16" y="26" style="font-size:15px">SP-05: 100,000 generated systems '
           '(universe seed 1; S1 + B1)</text>',
           '<rect class="s1" x="16" y="38" width="12" height="12" rx="2"/>'
           '<text class="legend" x="34" y="48">habitability bias 0.5 (all panels)</text>',
           '<rect class="s2" x="270" y="38" width="12" height="12" rx="2"/>'
           '<text class="legend" x="288" y="48">bias 0 (physics only; last two panels)</text>']
    for k, (name, title, mode) in enumerate(PANELS):
        sa = read(a / f"{name}.csv")
        series = [sa, read(b / f"{name}.csv")] if mode == "compare" else [sa]
        labels = [lab for lab, _ in sa]
        svg += panel((k % COLS) * W, 70 + (k // COLS) * H, title, series, labels)
    svg.append("</svg>")
    dest.write_text("\n".join(svg) + "\n")


if __name__ == "__main__":
    main()
