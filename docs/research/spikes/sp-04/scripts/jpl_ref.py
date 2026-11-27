# Independent reference: JPL "Approximate Positions of the Planets" Table 1 formulae (Standish), compared with
# Horizons DE441 heliocentric ecliptic-J2000 vectors. Usage: jpl_ref.py <horizons dir>
import math, sys, re

AU_KM = 149597870.7
T1 = {  # a, e, I, L, varpi, Omega ; rates per century
    "1": ([0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593],
          [0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081]),
    "2": ([0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255],
          [0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418]),
    "3": ([1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0],
          [0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0]),
    "4": ([1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891],
          [0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343]),
    "5": ([5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909],
          [-0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106]),
    "6": ([9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448],
          [-0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794]),
    "7": ([19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503],
          [-0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589]),
    "8": ([30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574],
          [0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664]),
}
STATED = {"1": (15, 1, 1), "2": (20, 1, 4), "3": (20, 8, 6), "4": (40, 2, 25), "5": (400, 10, 600),
          "6": (600, 25, 1500), "7": (50, 2, 1000), "8": (10, 1, 200)}
NAMES = {"1": "Mercury", "2": "Venus", "3": "EM Bary", "4": "Mars", "5": "Jupiter", "6": "Saturn", "7": "Uranus",
         "8": "Neptune"}

def kepler(Mdeg, e):
    M = math.radians(Mdeg)
    E = M + e * math.sin(M)
    for _ in range(50):
        E -= (E - e * math.sin(E) - M) / (1 - e * math.cos(E))
    return E

def position(el, T, secular=True, fixed_rate_L=True):
    base, rate = el
    k = [base[i] + (rate[i] * T if secular else 0.0) for i in range(6)]
    if not secular:
        k[3] = base[3] + rate[3] * T  # the mean longitude always advances
    a, e, I, L, wbar, Om = k
    w = wbar - Om
    M = L - wbar
    M = (M + 180.0) % 360.0 - 180.0
    E = kepler(M, e)
    xp = a * (math.cos(E) - e)
    yp = a * math.sqrt(1 - e * e) * math.sin(E)
    w, Om, I = math.radians(w), math.radians(Om), math.radians(I)
    x = (math.cos(w) * math.cos(Om) - math.sin(w) * math.sin(Om) * math.cos(I)) * xp + \
        (-math.sin(w) * math.cos(Om) - math.cos(w) * math.sin(Om) * math.cos(I)) * yp
    y = (math.cos(w) * math.sin(Om) + math.sin(w) * math.cos(Om) * math.cos(I)) * xp + \
        (-math.sin(w) * math.sin(Om) + math.cos(w) * math.cos(Om) * math.cos(I)) * yp
    z = math.sin(w) * math.sin(I) * xp + math.cos(w) * math.sin(I) * yp
    return [x * AU_KM, y * AU_KM, z * AU_KM]

def spherical(p):
    x, y, z = p
    r = math.sqrt(x * x + y * y + z * z)
    return math.atan2(y, x), math.asin(z / r), r

def horizons(path):
    t = open(path).read()
    body = t.split("$$SOE")[1].split("$$EOE")[0]
    rows = []
    for line in body.strip().splitlines():
        f = [s.strip() for s in line.split(",")]
        rows.append((float(f[0]), [float(f[2]), float(f[3]), float(f[4])]))
    return rows

if __name__ == "__main__":
    d = sys.argv[1]
    for mode in ("secular", "fixed"):
        print(f"== {mode} elements: max error over the six dates (lambda arcsec, phi arcsec, rho 1000 km) vs stated")
        for k in T1:
            worst = [0.0, 0.0, 0.0]
            for jd, h in horizons(f"{d}/h_{k}.txt"):
                T = (jd - 2451545.0) / 36525.0
                p = position(T1[k], T, secular=(mode == "secular"))
                l1, b1, r1 = spherical(p)
                l2, b2, r2 = spherical(h)
                dl = (l1 - l2 + math.pi) % (2 * math.pi) - math.pi
                worst[0] = max(worst[0], abs(math.degrees(dl)) * 3600)
                worst[1] = max(worst[1], abs(math.degrees(b1 - b2)) * 3600)
                worst[2] = max(worst[2], abs(r1 - r2) / 1000)
            s = STATED[k]
            ok = all(worst[i] <= s[i] for i in range(3))
            print(f"  {NAMES[k]:8s} {worst[0]:9.1f} {worst[1]:7.1f} {worst[2]:8.1f}   stated {s}  {'ok' if ok else 'EXCEEDS'}")
    # Values for the C++ test: Horizons vectors in km at each date.
    print("== Horizons vectors (km)")
    for k in list(T1) + ["399", "301"]:
        for jd, h in horizons(f"{d}/h_{k}.txt"):
            print(k, jd, ", ".join(f"{v:.9e}" for v in h))
