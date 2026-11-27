#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that GPU children cover their whole window under a fractional scale.

Usage (inside tests/isolated-run.py): gpu-child-edges.py GPU-CHILD-EDGES.exe
Build gpu-child-edges.c with winegcc -b x86_64-windows -ld3d11 -ldxgi first.
Every child, whatever its position and size modulo 5, must show as a solid
block exactly its size, or a device pixel larger at each edge that no
surface coordinate lands on, and never a pixel short: that showed a line of
its parent. The window is checked at several positions on the screen.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

import numpy
from office_windows import prefix_pids
from screen_area import ScreenArea

root = Path(__file__).resolve().parent.parent
if not os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"):
    sys.exit("Rendering tests require an isolated prefix")
exe = Path(sys.argv[1]).resolve()
CAPTION = "GPU child edges regression"


def kwin(body):
    with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as script:
        script.write("const pids=" + json.dumps(prefix_pids()) + ";for(const w of workspace.windowList())"
                     "{if(pids.includes(w.pid)&&w.caption===" + json.dumps(CAPTION) + "){" + body + "}}")
        script.flush()
        return sp.check_output([str(root / "tests/kwin-run.sh"), script.name], text=True)


def check(children, label):
    area = ScreenArea(0, 0, 2048, 1280)
    area.options["native-resolution"] = True
    image = ScreenArea.image(*area.capture()).convert("RGB")
    pixels = numpy.asarray(image).astype(int)
    red, green, blue = pixels[..., 0], pixels[..., 1], pixels[..., 2]
    boxes = {}
    origin = None
    for row, col in sorted(children):
        x, y, w, h = children[row, col]
        mask = ((blue >= 250) & (abs(red - (50 + 40 * col)) <= 2) &
                (abs(green - (50 + 40 * row)) <= 2))
        if origin:
            # only near where the child belongs: other windows share colours
            left, top = origin[0] + x - 3, origin[1] + y - 3
            region = numpy.zeros_like(mask)
            region[max(top, 0):top + h + 6, max(left, 0):left + w + 6] = True
            mask &= region
        ys, xs = numpy.nonzero(mask)
        if len(xs):
            boxes[row, col] = (xs.min(), ys.min(), xs.max(), ys.max())
            if not origin:
                origin = (xs.min() - x, ys.min() - y)
    failures = []
    for (row, col), (x, y, w, h) in children.items():
        box = boxes.get((row, col))
        if not box:
            failures.append(f"{label}: child {row},{col} not shown")
            continue
        shown_w, shown_h = box[2] - box[0] + 1, box[3] - box[1] + 1
        if not (w <= shown_w <= w + 2 and h <= shown_h <= h + 2):
            failures.append(f"{label}: child {row},{col} at {x},{y} size {w}x{h} shows {shown_w}x{shown_h}")
    image.save(Path(os.environ.get("TMPDIR", "/tmp")) / f"gpu-child-edges-{label}.png")
    return failures


with tempfile.TemporaryDirectory() as tmp:
    stop = Path(tmp) / "stop"
    process = sp.Popen([str(root / "tuxoffice"), "wine", "Z:" + str(exe).replace("/", "\\"),
                        "Z:" + str(stop).replace("/", "\\")], stdout=sp.PIPE, stderr=sp.DEVNULL, text=True)
    children, failures = {}, []
    try:
        for line in process.stdout:
            if line.startswith("child "):
                row, col, x, y, w, h = map(int, line.split()[1:])
                children[row, col] = (x, y, w, h)
            if line.startswith("READY"):
                break
        assert len(children) == 25, "the scene did not start"
        time.sleep(2)
        for offset in range(4):
            # logical positions that are and are not whole device pixels
            kwin(f"w.frameGeometry={{x:{200 + offset},y:{120 + offset},"
                 "width:w.frameGeometry.width,height:w.frameGeometry.height};")
            time.sleep(1)
            failures += check(children, f"offset{offset}")
    finally:
        stop.touch()
        process.wait(30)
print("\n".join(failures) or "gpu child edges: passed")
sys.exit(1 if failures else 0)
