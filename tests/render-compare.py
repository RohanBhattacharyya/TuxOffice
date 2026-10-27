#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Render one document position with two runners and compare the pixels.

Usage (inside tests/isolated-run.py): render-compare.py OUTDIR DOCUMENT RUNNER_A RUNNER_B [WHEEL_TICKS]
Each runner opens the document maximized, scrolls the same number of wheel
ticks, waits for the display to settle and captures the window. Compositor
optimizations must not change what Office shows.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

from PIL import Image, ImageChops
from office_windows import window_script
from screen_area import WindowCapture

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
document = Path(sys.argv[2]).resolve()
runners = sys.argv[3:5]
ticks = int(sys.argv[5]) if len(sys.argv) > 5 else 15
assert os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"), "Use the isolated prefix"
out.mkdir(parents=True, exist_ok=True)


def typer(env, *args):
    # Not pipes: processes Wine starts in the background inherit them and keep
    # them open, so reading a pipe to its end would never finish.
    with tempfile.TemporaryFile(mode="w+") as output:
        sp.run([str(root / "tuxoffice"), "wine", "C:/tuxoffice/typer.exe", "--app", "word", *map(str, args)],
               env=env, stdout=output, stderr=output, timeout=30)
        output.seek(0)
        return output.read()


images = []
for index, runner in enumerate(runners):
    env = dict(os.environ, TUXOFFICE_RUNNER=runner)
    sp.run([str(root / "tuxoffice"), "kill"], env=env, check=True)
    copy = out / f"compare-{index}{document.suffix}"
    copy.write_bytes(document.read_bytes())
    word = sp.Popen([str(root / "tuxoffice"), "run", "word", str(copy)], env=env,
                    stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    deadline = time.monotonic() + 90
    while not any(l.startswith("ok ") for l in typer(env, "ping", 700).splitlines()):
        assert time.monotonic() < deadline, "Word did not start"
        time.sleep(0.5)
    time.sleep(6)
    sp.run([sys.executable, str(root / "tests/activate-office.py"), "word", copy.stem], env=env, check=True)
    for command in (("maximize",), ("raise",), ("chord", "0x11", "0x24"), ("hover", 1050, 600)):
        typer(env, *command)
        time.sleep(0.5)
    for _ in range(ticks):
        typer(env, "wheel", -1)
        time.sleep(0.15)
    time.sleep(3)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as source:
        source.write(window_script("word", copy.stem))
        source.flush()
        text = sp.check_output([str(root / "tests/kwin-run.sh"), source.name], text=True)
    handle = next(l.split(" ", 1)[1] for l in text.splitlines() if l.startswith("TUXCAPTURE "))
    capture = WindowCapture(handle)
    info, data = capture.capture()
    image = capture.image(info, data).convert("RGB")
    image.save(out / f"runner-{index}.png")
    images.append(image)
    sp.run([str(root / "tuxoffice"), "kill"], env=env, check=True)
    word.wait(timeout=30)

a, b = images
assert a.size == b.size, f"Different window sizes {a.size} {b.size}"
diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 16 else 0)
changed = diff.histogram()[255]
box = diff.getbbox()
diff.save(out / "difference.png")
result = {"changed_pixels": changed, "changed_box": box, "size": a.size,
          "status": "identical" if changed == 0 else "different"}
(out / "summary.json").write_text(json.dumps(result) + "\n")
print(json.dumps(result))
