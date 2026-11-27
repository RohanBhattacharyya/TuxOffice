#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that one-pixel window region spans show their own pixel.

Usage (inside tests/isolated-run.py): region-span.py REGION-SPAN.exe
Build region-span.c with winegcc -b x86_64-windows first. Each spike above the
red body is one device pixel wide: it must show red, and the pixels beside it
must not.
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
CAPTION = "Region span regression"

with tempfile.TemporaryDirectory() as tmp:
    stop = Path(tmp) / "stop"
    process = sp.Popen([str(root / "tuxoffice"), "wine", "Z:" + str(exe).replace("/", "\\"),
                        "Z:" + str(stop).replace("/", "\\")], stdout=sp.PIPE, stderr=sp.DEVNULL, text=True)
    spikes = []
    try:
        for line in process.stdout:
            if line.startswith("spike "):
                spikes.append(int(line.split()[1]))
            if line.startswith("READY"):
                break
        assert len(spikes) == 8, "the scene did not start"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as script:
            script.write("const pids=" + json.dumps(prefix_pids()) + ";for(const w of workspace.windowList())"
                         "{if(pids.includes(w.pid)&&w.caption===" + json.dumps(CAPTION) + ")"
                         "{w.frameGeometry={x:300,y:200,width:w.frameGeometry.width,height:w.frameGeometry.height};}}")
            script.flush()
            sp.check_output([str(root / "tests/kwin-run.sh"), script.name], text=True)
        time.sleep(1.5)
        area = ScreenArea(0, 0, 2048, 1280)
        area.options["native-resolution"] = True
        image = ScreenArea.image(*area.capture()).convert("RGB")
        pixels = numpy.asarray(image).astype(int)
        red = (pixels[..., 0] >= 240) & (pixels[..., 1] <= 15) & (pixels[..., 2] <= 15)
        ys, xs = numpy.nonzero(red)
        assert len(xs), "the window is not shown"
        # the body: the widest red rows
        rows = numpy.bincount(ys)
        body_top = int(numpy.nonzero(rows >= 150)[0].min())
        left = int(xs[ys == body_top].min())
        failures = []
        for x in spikes:
            for y in range(body_top - 20, body_top):
                if not red[y, left + x]:
                    failures.append(f"spike {x} missing at row {y - body_top}")
                    break
            for dx in (1, 2, 3):
                if red[body_top - 10, left + x + dx]:
                    failures.append(f"pixel {dx} right of spike {x} shown")
                    break
        image.save(Path(os.environ.get("TMPDIR", "/tmp")) / "region-span.png")
    finally:
        stop.touch()
        process.wait(30)
print("\n".join(failures) or "region span: passed")
sys.exit(1 if failures else 0)
