#!/usr/bin/env python3
"""time.py <tree> <app lab|slice> <script|-> <size> [ENV=v ...]: GPU ms per pass (timings.py), the sky passes summed."""
import json
import os
import subprocess
import sys

tree, app, script, size = sys.argv[1:5]
env = dict(os.environ)
args = []
args = ["--warmup", os.environ.get("WARMUP", "60"), "--samples", os.environ.get("SAMPLES", "60")]
for kv in sys.argv[5:]:
    args += ["--env", kv]
cmd = [sys.executable, os.path.join(tree, "docs/research/gpu-driven/scripts/timings.py"), "--app",
       os.path.join(tree, f"build/release/apps/{app}/veach-{app}"), "--project", os.path.join(tree, "apps", app),
       "--size", size] + (["--script", script] if script != "-" else []) + args
out = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, env=env)
txt = out.stdout
try:
    r = json.loads(txt[txt.index("{"):])
except ValueError:
    print(out.stdout[-2000:], out.stderr[-3000:])
    sys.exit(1)
ps = r["passes"]
print("total", round(r["total_ms"], 3), "frame", r.get("frame_ms"))
for k, v in sorted(ps.items(), key=lambda x: -x[1]):
    if v > 0.004 and (k.startswith("sky") or k in ("shade", "visibility", "ao: trace") or v > 0.15):
        print(f"{k:40s} {v:.3f}")
print("sky passes sum", round(sum(v for k, v in ps.items() if k.startswith("sky")), 3))
