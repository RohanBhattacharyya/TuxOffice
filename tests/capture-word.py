#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Capture Word by KWin window ID, even when another application has focus."""
import os
import json
from pathlib import Path
import subprocess
import sys
import threading
import tempfile

import dbus
from PIL import Image
from office_windows import classes,prefix_pids,window_script

here = Path(__file__).resolve().parent
output = Path(sys.argv[1])
app = sys.argv[2] if len(sys.argv) > 2 else "word"
if app == "--title":
    script = 'const pids='+json.dumps(prefix_pids())+'; for(const w of workspace.windowList()){if(pids.includes(w.pid) && w.caption === '+json.dumps(sys.argv[3])+') print("TUXCAPTURE",w.internalId);}'
elif app not in classes:
    sys.exit("Unknown Office app")
else:
    script = window_script(app)
if app in classes and "--active" in sys.argv[3:]:
    script = 'const pids='+json.dumps(prefix_pids(app))+'; const w=workspace.activeWindow; if(w && pids.includes(w.pid) && w.resourceClass === '+json.dumps(classes[app])+') print("TUXCAPTURE",w.internalId);'
with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as source:
    source.write(script)
    source.flush()
    info = subprocess.check_output([str(here / "kwin-run.sh"), source.name], text=True)
handles = [line.split()[1] for line in info.splitlines() if line.startswith("TUXCAPTURE ")]
if not handles:
    sys.exit("Office app window not found")
read_fd, write_fd = os.pipe()
chunks = []


def drain():
    with os.fdopen(read_fd, "rb") as stream:
        while chunk := stream.read(1024 * 1024):
            chunks.append(chunk)


reader = threading.Thread(target=drain, daemon=True)
reader.start()
try:
    obj = dbus.SessionBus().get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2")
    result = obj.CaptureWindow(
        handles[-1], dbus.Dictionary({"include-decoration": False}, signature="sv"),
        dbus.types.UnixFd(write_fd), dbus_interface="org.kde.KWin.ScreenShot2", timeout=10,
    )
finally:
    os.close(write_fd)
reader.join(10)
if reader.is_alive():
    sys.exit("Screenshot pipe timed out")
formats = {4: "BGRX", 5: "BGRA", 6: "BGRA", 16: "RGBX", 17: "RGBA", 18: "RGBA"}
pixel_format = formats.get(int(result["format"]))
if not pixel_format:
    sys.exit(f"Unsupported QImage pixel format: {result['format']}")
mode = "RGB" if int(result["format"]) in (4,16) else "RGBA"
image = Image.frombytes(mode, (int(result["width"]), int(result["height"])),
                        b"".join(chunks), "raw", pixel_format, int(result["stride"]))
image.save(output)
print(output)
