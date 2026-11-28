#!/usr/bin/env python3
"""Spike SP-10 (VCH-086): render the screenshot set of docs/research/spikes/sp-10-atmosphere.md.

  python3 docs/research/spikes/sp-10/scripts/shots.py --lab build/release/apps/lab/veach-lab \
      --out docs/research/spikes/sp-10 [--only PREFIX]

Each shot is one headless run of the lab with VEACH_SP10_* set (apps/lab/src/sp10_spike.hpp). A run that logs an
error (non-zero exit) stops the script.
"""
import argparse
import os
import subprocess
import sys

ALTITUDES = [("ground", "2"), ("1km", "1000"), ("10km", "10000"), ("100km", "100000"), ("top-", "top-"),
             ("top+", "top+"), ("orbit", "orbit")]
EV = {"earth": 15.0, "mars": 14.0, "titan": 9.0, "hillaire": 15.0}


def shots() -> list[tuple[str, dict[str, str]]]:
    out: list[tuple[str, dict[str, str]]] = []
    for comp in ["earth", "mars"]:
        for scale in ["1", "0.01"]:
            for name, altitude in ALTITUDES:
                env = {"VEACH_SP10": comp, "VEACH_SP10_SCALE": scale, "VEACH_SP10_ALTITUDE": altitude,
                       "VEACH_SP10_EV": str(EV[comp])}
                out.append((f"{comp}_s{scale}_{name}", env))
            # The seam: at the top's inside, the sky-view LUT against the per-pixel march; on the ground, the same.
            for name, altitude in [("top-", "top-"), ("ground", "2")]:
                for march in ["never", "always"]:
                    env = {"VEACH_SP10": comp, "VEACH_SP10_SCALE": scale, "VEACH_SP10_ALTITUDE": altitude,
                           "VEACH_SP10_EV": str(EV[comp]), "VEACH_SP10_MARCH": march}
                    out.append((f"seam/{comp}_s{scale}_{name}_{march}", env))
    # Sunsets: the horizontal optical depth does not follow s_a (it goes as s_p^(1/4)).
    for comp in ["earth", "mars"]:
        for scale in ["1", "0.01"]:
            out.append((f"sunset/{comp}_s{scale}", {"VEACH_SP10": comp, "VEACH_SP10_SCALE": scale,
                                                    "VEACH_SP10_SUN_ELEVATION": "1.5", "VEACH_SP10_SUN_AZIMUTH": "0",
                                                    "VEACH_SP10_PITCH": "8", "VEACH_SP10_FOV": "70",
                                                    "VEACH_SP10_EV": "12" if comp == "earth" else "11"}))
    # The s_a rule against s_a = s_p (exponent 1) and s_p^0.75, at s = 0.01.
    for exponent in ["1", "0.75"]:
        base = {"VEACH_SP10": "earth", "VEACH_SP10_SCALE": "0.01", "VEACH_SP10_SA_EXPONENT": exponent}
        out.append((f"sa/earth_e{exponent}_ground", {**base, "VEACH_SP10_EV": "15"}))
        out.append((f"sa/earth_e{exponent}_orbit", {**base, "VEACH_SP10_ALTITUDE": "orbit", "VEACH_SP10_EV": "15"}))
        out.append((f"sa/earth_e{exponent}_sunset", {**base, "VEACH_SP10_SUN_ELEVATION": "1.5",
                                                     "VEACH_SP10_SUN_AZIMUTH": "0", "VEACH_SP10_PITCH": "8",
                                                     "VEACH_SP10_FOV": "70", "VEACH_SP10_EV": "12"}))
    # Fewer march steps. VEACH_SP10_STEPS sets the sky-view LUT's steps too, which only matters inside the atmosphere.
    for scale in ["1", "0.01"]:
        for name, altitude in [("top+", "top+"), ("orbit", "orbit")]:
            out.append((f"steps/earth_s{scale}_{name}_16", {"VEACH_SP10": "earth", "VEACH_SP10_SCALE": scale,
                                                            "VEACH_SP10_ALTITUDE": altitude, "VEACH_SP10_STEPS": "16",
                                                            "VEACH_SP10_EV": "15"}))
    # Composition against Hillaire's Earth, the same view.
    out.append(("compare/hillaire_s1_ground", {"VEACH_SP10": "hillaire", "VEACH_SP10_ALTITUDE": "2"}))
    out.append(("compare/earth_s1_ground", {"VEACH_SP10": "earth", "VEACH_SP10_ALTITUDE": "2"}))
    # Two suns.
    out.append(("suns/system_sunset", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "system", "VEACH_SP10_HELMET": "1",
                                       "VEACH_SP10_PITCH": "4", "VEACH_SP10_EV": "11"}))
    out.append(("suns/wide_ground", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "wide", "VEACH_SP10_HELMET": "1",
                                     "VEACH_SP10_FOV": "100", "VEACH_SP10_YAW": "-15"}))
    out.append(("suns/wide_10km", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "wide", "VEACH_SP10_ALTITUDE": "10000",
                                   "VEACH_SP10_FOV": "100", "VEACH_SP10_YAW": "-15"}))
    # Facing the sky-view LUT's azimuth wrap (opposite the first sun), where its two edges meet.
    out.append(("suns/wide_wrap", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "wide", "VEACH_SP10_YAW": "220",
                                   "VEACH_SP10_PITCH": "20", "VEACH_SP10_FOV": "90"}))
    out.append(("suns/wide_orbit", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": "wide", "VEACH_SP10_ALTITUDE": "orbit"}))
    out.append(("suns/wide_mars_s0.01_ground", {"VEACH_SP10": "mars", "VEACH_SP10_SCALE": "0.01",
                                                "VEACH_SP10_SUNS": "wide", "VEACH_SP10_FOV": "100", "VEACH_SP10_YAW": "-15", "VEACH_SP10_EV": "14"}))
    # Eclipses: the first sun behind a moon 15% larger than its disk.
    for name, suns, eclipse, ev in [("one_total", "one", "0", "15"), ("one_total_ev9", "one", "0", "9"),
                                    ("one_partial", "one", "0.5", "15"), ("wide_total", "wide", "0", "15"),
                                    ("wide_total_ev11", "wide", "0", "11")]:
        out.append((f"eclipse/{name}", {"VEACH_SP10": "earth", "VEACH_SP10_SUNS": suns, "VEACH_SP10_ECLIPSE": eclipse,
                                        "VEACH_SP10_SUN_ELEVATION": "35", "VEACH_SP10_SUN_AZIMUTH": "30",
                                        "VEACH_SP10_FOV": "90", "VEACH_SP10_EV": ev}))
    out.append(("eclipse/one_total_orbit", {"VEACH_SP10": "earth", "VEACH_SP10_ECLIPSE": "0",
                                            "VEACH_SP10_ALTITUDE": "orbit", "VEACH_SP10_SUN_ELEVATION": "35"}))
    # A dense, hazy atmosphere: Titan-like.
    for scale in ["1", "0.01"]:
        for name, altitude in [("ground", "2"), ("orbit", "orbit")]:
            out.append((f"titan/titan_s{scale}_{name}", {"VEACH_SP10": "titan", "VEACH_SP10_SCALE": scale,
                                                         "VEACH_SP10_ALTITUDE": altitude, "VEACH_SP10_EV": "9"}))
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--only", default="")
    args = parser.parse_args()
    lab = os.path.abspath(args.lab)
    project = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../../apps/lab")
    for name, env in shots():
        if not name.startswith(args.only):
            continue
        path = os.path.abspath(os.path.join(args.out, name + ".png"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        run_env = {k: v for k, v in os.environ.items() if not k.startswith("VEACH_SP10")}
        run_env.update(env)
        result = subprocess.run([lab, "--rhi", "metal", "--headless", "--frames", "4", "--screenshot", path],
                                cwd=project, env=run_env, capture_output=True, text=True)
        info = [line for line in result.stdout.splitlines() if "sp10" in line]
        print(name, *info, sep="\n  ")
        if result.returncode != 0:
            print(result.stdout[-3000:], result.stderr[-3000:])
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
