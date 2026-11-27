#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Measure input-to-frame latency and frame count of an Office document view.

Usage (inside tests/isolated-run.py): input-latency.py LOG [wheel|type] [X Y]
Start the app first with WINEDEBUG=+timestamp,+uiperf,+waylanddrv,+key and its
output in LOG, with a long document whose view is at logical (X, Y), by
default (1000, 700); PowerPoint's slide thumbnails are at (376, 700) with the
window placed as the Office suite places it. The view is clicked and sent to
its start; then 90 wheel steps at about 60 a second,
or 88 typed characters, are sent. Reports the composition target drawn most
often (the document view), its draws, the longest gaps between them, and the
time from each input event reaching Wine to that target's next draw.
"""
import os
from pathlib import Path
import re
import statistics
import subprocess as sp
import sys
import tempfile
import time
import collections

root = Path(__file__).resolve().parent.parent
if not os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"):
    sys.exit("This test requires an isolated Office prefix")
log = sys.argv[1]; mode = sys.argv[2] if len(sys.argv) > 2 else "wheel"
x, y = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (1000, 700)
build = Path(tempfile.mkdtemp())
eis_input = build / "eis-input"
flags = sp.check_output(["pkg-config", "--cflags", "--libs", "libei-1.0", "libsystemd"], text=True).split()
sp.run(["cc", "-O2", "-o", str(eis_input), str(root / "tests/eis-input.c"), *flags], check=True)
def eis(lines):
    sp.run([str(eis_input)], input="\n".join(lines) + "\n", text=True)
def keys(item):
    return sp.run([sys.executable, str(root / "tests/eis-type.py"), item], capture_output=True, text=True).stdout.splitlines()
eis([f"move {x} {y}", "sleep 100", "button 272 1", "sleep 30", "button 272 0", "sleep 300"] + keys("{ctrl+home}") + ["sleep 1500"])
mark = sum(1 for _ in open(log, errors="replace"))
if mode == "wheel":
    eis([f"move {x} {y}", "sleep 200"] + [c for i in range(90) for c in ("wheel 1", "sleep 16")] + ["sleep 1500"])
else:
    seq = []
    for ch in "the quick brown fox jumps over the lazy dog " * 2:
        seq += keys(ch) + ["sleep 60"]
    eis(["sleep 200"] + seq + ["sleep 1500"])
time.sleep(0.5)
L = open(log, errors="replace").read().splitlines()[mark:]
ev, draws, target = [], collections.defaultdict(list), None
for l in L:
    m = re.match(r"(\d+\.\d+):(\w+):trace:(\w+):(\w+) (.*)", l)
    if not m: continue
    t = float(m.group(1)) * 1000; f = m.group(4); msg = m.group(5)
    if f == "send_scroll": ev.append(t)
    elif m.group(3) == "key" and "state=0x1" in msg: ev.append(t)
    elif f == "desktop_window_target_render_locked" and msg.startswith("frame hwnd") and "unchanged" not in msg and " tick " in msg:
        draws[msg.split()[2]].append(t)
if not ev: sys.exit("no input events seen")
canvas = max(draws, key=lambda h: len([t for t in draws[h] if ev[0] <= t <= ev[-1] + 300]))
d = [t for t in draws[canvas] if ev[0] - 5 <= t <= ev[-1] + 600]
gaps = sorted((b - a for a, b in zip(d, d[1:])), reverse=True)
lat = []
for t in ev:
    n = [x for x in d if x >= t]
    if n: lat.append(n[0] - t)
lat.sort()
print(f"{mode}: {len(ev)} events over {ev[-1]-ev[0]:.0f} ms; canvas {canvas} {len(d)} draws; "
      f"longest gaps {[round(g) for g in gaps[:4]]}; latency median {statistics.median(lat):.0f} p90 {lat[int(len(lat)*.9)]:.0f} max {lat[-1]:.0f} ms")
