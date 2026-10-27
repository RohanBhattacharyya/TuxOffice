#!/usr/bin/env python3
"""Resize an Office window with KDE's Meta+right-drag and check its frame shadow.

Usage (inside tests/isolated-run.py, with tests/backdrop.qml showing):
    shadow-resize.py OUTDIR APP [TRIALS]
Input goes to the isolated KWin only, through tests/eis-input (libei). After
each drag the shadow must sit directly against all four frame edges.
"""
import json
import os
from pathlib import Path
import random
import subprocess as sp
import sys
import tempfile
import time

from office_windows import window_script
from screen_area import ScreenArea

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
app = sys.argv[2]
trials = int(sys.argv[3]) if len(sys.argv) > 3 else 24
eis = Path(os.environ.get("TUX_EIS_INPUT", Path.home() / ".cache/tuxoffice-validation/bin/eis-input"))
assert os.environ.get("WAYLAND_DISPLAY", "").startswith("tuxoffice-test-"), "Run inside the isolated session"
out.mkdir(parents=True, exist_ok=True)
random.seed(int(os.environ.get("TUX_SEED", "1")))


def kwin(script):
    with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as source:
        source.write(script)
        source.flush()
        return sp.check_output([str(root / "tests/kwin-run.sh"), source.name], text=True)


def geometry():
    text = kwin(window_script(app, activate=True, geometry=True))
    return json.loads(next(l.split(" ", 1)[1] for l in text.splitlines() if l.startswith("TUXGEOMETRY ")))


def send(commands):
    sp.run([str(eis)], input="\n".join(commands) + "\n", text=True, check=True, timeout=60)


def shadow_ok(image, g):
    """Shadow pixels just outside each edge are darker than the backdrop, and
    the backdrop a few pixels further out is untouched."""
    x0, y0 = round(g["x"]), round(g["y"])
    x1, y1 = x0 + round(g["width"]), y0 + round(g["height"])
    W, H = image.size
    problems = []
    for name, inside, outside in (
            ("left", lambda t: (x0 - t, (y0 + y1) // 2), x0 > 12),
            ("right", lambda t: (x1 - 1 + t, (y0 + y1) // 2), x1 < W - 12),
            ("top", lambda t: ((x0 + x1) // 2, y0 - t), y0 > 12),
            ("bottom", lambda t: ((x0 + x1) // 2, y1 - 1 + t), y1 < H - 12)):
        if not outside:
            continue
        near = image.getpixel(inside(2))
        far = image.getpixel(inside(11))
        if near > 225 or far < 250:
            problems.append(f"{name}: near {near} far {far}")
    return problems


# TUX_DRAGS="col,row,dx,dy,steps;..." replays chosen drags instead of random ones.
drags = [tuple(int(v) for v in d.split(",")) for d in os.environ.get("TUX_DRAGS", "").split(";") if d]
if drags:
    trials = len(drags)
results = []
backdrop = ScreenArea(0, 0, 2048, 1280)
for trial in range(trials):
    g = geometry()
    # Keep the window inside the screen so every edge can be inspected.
    if g["width"] > 1700 or g["height"] > 1100 or g["x"] < 40 or g["y"] < 40 or \
            g["x"] + g["width"] > 2008 or g["y"] + g["height"] > 1240:
        kwin(window_script(app) + 'if(w){w.setMaximize(false,false);w.frameGeometry={x:300,y:200,width:1300,height:800};}')
        time.sleep(1)
        g = geometry()
    col, row = random.choice(((0, 0), (2, 0), (0, 2), (2, 2), (1, 0), (1, 2), (0, 1), (2, 1)))
    if drags:
        col, row, dx, dy, steps = drags[trial]
    sx = g["x"] + g["width"] * (col * 2 + 1) / 6
    sy = g["y"] + g["height"] * (row * 2 + 1) / 6
    if not drags:
        dx = random.choice((-1, 1)) * random.randint(30, 180) if col != 1 else 0
        dy = random.choice((-1, 1)) * random.randint(30, 140) if row != 1 else 0
        steps = random.choice((4, 12, 40))
    commands = [f"move {sx:.0f} {sy:.0f}", "sleep 80", "key 125 1", "sleep 40", "button 273 1", "sleep 40"]
    for i in range(1, steps + 1):
        commands += [f"move {sx + dx * i / steps:.1f} {sy + dy * i / steps:.1f}", "sleep 16"]
    commands += ["sleep 60", "button 273 0", "sleep 20", "key 125 0"]
    send(commands)
    time.sleep(0.8)
    after = geometry()
    info, data = backdrop.capture()
    image = backdrop.image(info, data).convert("L")
    problems = shadow_ok(image, after)
    row_result = {"trial": trial, "region": [col, row], "delta": [dx, dy], "steps": steps,
                  "before": g, "after": after, "problems": problems}
    if problems:
        image.save(out / f"misplaced-{trial}.png")
    results.append(row_result)
    print(json.dumps(row_result), flush=True)
failed = [r for r in results if r["problems"]]
summary = {"app": app, "trials": trials, "misplaced": len(failed), "status": "passed" if not failed else "failed"}
(out / "summary.json").write_text(json.dumps({"summary": summary, "trials": results}, indent=1) + "\n")
print(json.dumps(summary))
sys.exit(1 if failed else 0)
