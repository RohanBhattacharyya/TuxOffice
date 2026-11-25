#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Insert a picture through PowerPoint's Online Pictures dialog (WebView2).

Usage: isolated-run.py SESSION python3 tests/webview2-office.py OUTDIR
Needs a signed-in Office with the WebView2 runtime, network access to Bing,
and the helpers from tests/build-helpers.sh. Clicks are at fixed positions of
a maximized PowerPoint on the isolated session's 2560x1600 screen.

Checks that the web page renders in the dialog (not blank), takes typing, and
that the chosen picture lands on the slide.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import time

from PIL import Image

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
prefix = Path(os.environ.get("TUXOFFICE_PREFIX", ""))
assert str(prefix).endswith("/home/prefix"), "Use an isolated Office prefix"
work = prefix / "drive_c/tuxoffice/webview2-office"
work.mkdir(parents=True, exist_ok=True)
status = work / "status.txt"
title = "Presentation1 - PowerPoint"
checks = {}


def wine(*args, timeout=30):
    return sp.run([str(root / "tuxoffice"), "wine", *args], capture_output=True, text=True,
                  errors="replace", timeout=timeout)


def click(window, x, y, width=2560, height=1600):
    result = wine("C:/tuxoffice/winctl.exe", "click-relative", window, str(x), str(y), str(width), str(height))
    assert "clicked" in result.stdout, result.stdout + result.stderr


def capture(name, *window):
    path = out / f"{name}.png"
    sp.run([sys.executable, str(root / "tests/capture-word.py"), str(path), *window], check=True,
           stdout=sp.DEVNULL, stderr=sp.DEVNULL, timeout=30)
    return Image.open(path).convert("RGB")


def wait_for(predicate, seconds, what):
    deadline = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < deadline, what
        time.sleep(0.5)


def dialog_open():
    return "WebView2Dialog_v2" in wine("C:/tuxoffice/winctl.exe", "list").stdout


(work / "start.vbs").write_text(r'''Set app = CreateObject("PowerPoint.Application")
app.Visible = True
app.Presentations.Add.Slides.Add 1, 12
app.WindowState = 3
Set f = CreateObject("Scripting.FileSystemObject").CreateTextFile(WScript.Arguments(0), True)
f.WriteLine "READY"
f.Close
''')
(work / "count.vbs").write_text(r'''Set app = CreateObject("PowerPoint.Application")
Set f = CreateObject("Scripting.FileSystemObject").CreateTextFile(WScript.Arguments(0), True)
f.WriteLine "SHAPES " & app.ActivePresentation.Slides(1).Shapes.Count
f.Close
''')

sp.run([str(root / "tuxoffice"), "kill"], check=False)
status.unlink(missing_ok=True)
client = sp.Popen([str(root / "tuxoffice"), "wine", "cscript", "//nologo", r"C:\tuxoffice\webview2-office\start.vbs",
                   r"C:\tuxoffice\webview2-office\status.txt"], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
try:
    wait_for(lambda: status.exists() and "READY" in status.read_text(), 120, "PowerPoint did not start")
    time.sleep(4)
    click(title, 173, 84)          # Insert tab
    time.sleep(2)
    click(title, 199, 170)         # Pictures
    time.sleep(2)
    click(title, 272, 369)         # Online Pictures...
    wait_for(dialog_open, 40, "Online Pictures dialog did not open")
    time.sleep(15)
    page = capture("dialog", "--title", "Insert Picture")
    colors = len(page.resize((64, 40)).getcolors(64 * 40))
    checks["page renders"] = colors > 200
    result = wine("C:/tuxoffice/typer.exe", "--app", "powerpoint", "type", "lighthouse")
    wine("C:/tuxoffice/typer.exe", "--app", "powerpoint", "key", "13")
    checks["typing accepted"] = result.returncode == 0
    time.sleep(10)
    results = capture("results", "--title", "Insert Picture")
    checks["search shows results"] = results.tobytes() != page.tobytes()
    width, height = results.size
    click("Insert Picture", 130, 366, width, height)      # first result
    time.sleep(2)
    click("Insert Picture", 1744, 1240, width, height)    # Insert
    wait_for(lambda: not dialog_open(), 60, "Online Pictures dialog did not close")
    time.sleep(5)
    status.unlink(missing_ok=True)
    wine("cscript", "//nologo", r"C:\tuxoffice\webview2-office\count.vbs", r"C:\tuxoffice\webview2-office\status.txt",
         timeout=60)
    shapes = status.read_text().split()[-1] if status.exists() else "0"
    checks["picture inserted"] = int(shapes) >= 1
    capture("slide", "powerpoint")
except Exception as error:
    checks["completed"] = False
    (out / "error.txt").write_text(str(error))
finally:
    sp.run([str(root / "tuxoffice"), "kill"], check=False)
    try:
        client.wait(timeout=20)
    except sp.TimeoutExpired:
        client.kill()

summary = {"status": "passed" if checks and all(checks.values()) else "failed", "checks": checks}
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
sys.exit(summary["status"] != "passed")
