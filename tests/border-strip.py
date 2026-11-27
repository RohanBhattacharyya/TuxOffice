#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that an Office-style border-effect strip shows beside its frame.

Usage (inside tests/isolated-run.py): border-strip.py BORDER-STRIP.exe
Build border-strip.c with winegcc -b x86_64-windows first. The strip must be
red in its top half and blend to grey over the white frame's backdrop in its
bottom half, directly right of the frame.
"""
import os
from pathlib import Path
import subprocess as sp
import sys
import time

import dbus
from screen_area import ScreenArea

root = Path(__file__).resolve().parent.parent
if not os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"):
    sys.exit("Rendering tests require an isolated prefix")
exe = Path(sys.argv[1]).resolve()
process = sp.Popen([str(root / "tuxoffice"), "wine", "Z:" + str(exe).replace("/", "\\"), "8"],
                   stdout=sp.DEVNULL, stderr=sp.DEVNULL)
try:
    time.sleep(5)
    area = ScreenArea(0, 0, 8192, 8192)
    area.options["native-resolution"] = True
    image = ScreenArea.image(*area.capture()).convert("RGB")
finally:
    process.wait(30)
pixels, (width, height) = image.load(), image.size
red = [(x, y) for y in range(0, height, 2) for x in range(width)
       if pixels[x, y][0] > 200 and pixels[x, y][1] < 60 and pixels[x, y][2] < 60]
# The strip's red half is a column 200 device pixels tall; other red on
# the screen (an Office icon) is not.
columns = {}
for x, y in red:
    columns.setdefault(x, []).append(y)
strip = {x: ys for x, ys in columns.items() if len(ys) >= 80}
assert strip, "the border strip is not shown"
left = min(strip)
bottom = max(strip[left])
assert pixels[left - 2, bottom] == (255, 255, 255), "the strip is not beside the white frame"
half = pixels[left + 4, bottom + 40]
assert all(100 < c < 160 for c in half), f"the strip's translucent half shows {half}"
print(f"border strip shown at x {left}, translucent half {half}")
