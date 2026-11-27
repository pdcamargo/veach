# Writes tests/support/sp04_sol_reference.inl: JPL Table 1 formula positions (computed here, independently of the
# C++) and JPL Horizons DE441 positions at six dates. Usage: gen_inl.py <horizons dir> <out.inl>
import math, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from jpl_ref import T1, NAMES, horizons, position, kepler, spherical

def moon_geo(jd):
    a, e, w0, M0, I, Om0 = 384400.0, 0.0554, 318.15, 135.27, 5.16, 125.08
    P, Pw, Pn = 27.322, 5.997 * 365.25, 18.600 * 365.25
    d = jd - 2451545.0
    wdot, Omdot = 360.0 / Pw, -360.0 / Pn
    Ldot = 360.0 / P
    L = Om0 + w0 + M0 + Ldot * d
    Om = Om0 + Omdot * d
    wbar = Om0 + w0 + (wdot + Omdot) * d
    M = (L - wbar + 180.0) % 360.0 - 180.0
    E = kepler(M, e)
    xp, yp = a * (math.cos(E) - e), a * math.sqrt(1 - e * e) * math.sin(E)
    w, Om, i = math.radians(wbar - Om), math.radians(Om), math.radians(I)
    x = (math.cos(w) * math.cos(Om) - math.sin(w) * math.sin(Om) * math.cos(i)) * xp + (-math.sin(w) * math.cos(Om) - math.cos(w) * math.sin(Om) * math.cos(i)) * yp
    y = (math.cos(w) * math.sin(Om) + math.sin(w) * math.cos(Om) * math.cos(i)) * xp + (-math.sin(w) * math.sin(Om) + math.cos(w) * math.cos(Om) * math.cos(i)) * yp
    z = math.sin(w) * math.sin(i) * xp + math.cos(w) * math.sin(i) * yp
    return [x, y, z]

mu = 1.0 / (1.0 + 81.30056)
names = {"1": "mercury", "2": "venus", "3": "emb", "4": "mars", "5": "jupiter", "6": "saturn", "7": "uranus",
         "8": "neptune", "399": "earth", "301": "moon"}
d = sys.argv[1]
lines = ["// Generated for spike SP-04 (VCH-080) by the script described in docs/research/spikes/sp-04-orrery-clock.md.",
         "// formula: JPL Table 1 (and the Moon's mean elements) evaluated in Python, independently of veach::space.",
         "// de441: JPL Horizons vectors (DE441), heliocentric (the Moon geocentric), ecliptic of J2000, TDB; fetched",
         "// 2026-11-27 from https://ssd.jpl.nasa.gov/api/horizons.api. Kilometers.",
         "// clang-format off"]
for k in ["1", "2", "3", "4", "5", "6", "7", "8", "399", "301"]:
    for jd, h in horizons(f"{d}/h_{k}.txt"):
        T = (jd - 2451545.0) / 36525.0
        if k in T1:
            f = position(T1[k], T)
        elif k == "301":
            f = moon_geo(jd)
        else:
            emb = position(T1["3"], T)
            m = moon_geo(jd)
            f = [emb[i] - mu * m[i] for i in range(3)]
        lines.append(f'{{"{names[k]}", {jd:.1f}, {{{f[0]:.17g}, {f[1]:.17g}, {f[2]:.17g}}}, '
                     f'{{{h[0]:.15e}, {h[1]:.15e}, {h[2]:.15e}}}}},')
lines.append("// clang-format on")
open(sys.argv[2], "w").write("\n".join(lines) + "\n")
print(len(lines))
