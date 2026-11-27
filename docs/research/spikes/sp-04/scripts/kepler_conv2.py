# Kepler solver convergence: iterations a starter needs to reach the double's limit (SP-04). Usage: kepler_conv2.py
import math, random

def ref(M, e):
    lo, hi = 0.0, math.pi
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if mid - e * math.sin(mid) - M > 0: hi = mid
        else: lo = mid
    return 0.5 * (lo + hi)

def newton(M, e, E, n):
    for _ in range(n):
        E = E - (E - e * math.sin(E) - M) / (1 - e * math.cos(E))
    return E

def starter(M, e):
    return min(math.pi, (6 * M) ** (1 / 3)) if e > 0.8 else M + e * math.sin(M)

random.seed(1)
worst = {}
for trial in range(200000):
    e = random.choice([random.random() * 0.8, 0.8 + random.random() * 0.1999, 1 - 10 ** random.uniform(-4, -1)])
    e = min(e, 0.9999)
    M = random.choice([random.random() * math.pi, 10 ** random.uniform(-15, 0), math.pi - 10 ** random.uniform(-12, 0)])
    r = ref(M, e)
    E0 = starter(M, e)
    for n in range(0, 60):
        if abs(newton(M, e, E0, n) - r) <= max(2e-15 * max(1.0, r), 2 * abs(newton(M, e, E0, 80) - r)):
            break
    band = "e<0.7" if e < 0.7 else ("e<=0.8" if e <= 0.8 else "e>0.8")
    if n > worst.get(band, (0,))[0]:
        worst[band] = (n, e, M)
print(worst)
