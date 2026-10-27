#!/usr/bin/env python3
"""Check scrolling and responsiveness with all three Office apps open.

Usage: multi-app-smoke.py OUTDIR WORD.docx EXCEL.xlsx POWERPOINT.pptx
Requires the disposable Office prefix and scrolling benchmark fixtures.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time
import zlib

from screen_area import ScreenArea

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
prefix = os.environ.get("TUXOFFICE_PREFIX", "")
if not prefix.endswith("/home/prefix"):
    sys.exit("This test requires an isolated Office prefix")
documents = dict(zip(("word", "excel", "powerpoint"), map(lambda x:Path(x).resolve(), sys.argv[2:])))
if len(documents) != 3 or not all(p.is_file() for p in documents.values()):
    sys.exit("Provide all three scrolling fixtures")
out.mkdir(parents=True, exist_ok=True)
results = []


def helper(app, *args):
    with tempfile.TemporaryFile(mode="w+") as log:
        process = sp.run([str(root/"tuxoffice"), "wine", "C:/tuxoffice/typer.exe", "--app", app,
                          *map(str,args)], stdout=log, stderr=log, timeout=15)
        log.seek(0)
        text = log.read()
    with (out/"actions.log").open("a") as log:
        log.write(f"{app} {args} exit={process.returncode}\n{text}")
    return process.returncode, text


def ping(app):
    status, text = helper(app, "ping", 1000)
    return status == 0 and any(line.startswith("ok ") for line in text.splitlines())


try:
    sp.run([str(root/"tuxoffice"), "kill"], check=True)
    # A crash in an earlier run makes Office ask about safe mode at startup.
    for name in ("Word", "Excel", "PowerPoint"):
        sp.run([str(root/"tuxoffice"), "wine", "reg", "delete",
                rf"HKCU\Software\Microsoft\Office\16.0\{name}\Resiliency", "/f"],
               stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    sp.run([str(root/"tuxoffice"), "kill"], check=True)
    for app, document in documents.items():
        env = dict(os.environ, WINEDEBUG="-all,err+all,warn+seh")
        with (out/(app+".log")).open("w") as log:
            sp.Popen([str(root/"tuxoffice"), "run", app, str(document)], stdout=log, stderr=log, env=env)
        deadline = time.monotonic()+90
        while not ping(app):
            if time.monotonic() > deadline:
                raise RuntimeError(app+" did not become ready")
            if app == "word": helper(app,"dismiss-startup")
            time.sleep(.5)
    time.sleep(10)
    for round_number in range(2):
        for app, document in documents.items():
            sp.run([sys.executable,str(root/"tests/activate-office.py"),app,document.stem],check=True)
            deadline = time.monotonic()+5
            while document.stem not in helper(app,"foreground")[1]:
                if time.monotonic() > deadline: raise RuntimeError("Incorrect foreground document")
                time.sleep(.1)
            for args in [("maximize",),("raise",),("chord","0x11","0x24"),
                         ("hover",120 if app=="powerpoint" else 1050,600)]:
                if helper(app,*args)[0]: raise RuntimeError("Could not prepare "+app)
            time.sleep(2)
            if document.stem not in helper(app,"hit-test")[1]:
                raise RuntimeError("Mouse would target another document")
            x,y = {"word":(704,340),"excel":(60,260),"powerpoint":(40,260)}[app]
            area = ScreenArea(x,y,320,180)
            before_info,before = area.capture()
            if helper(app,"wheel",-12)[0]: raise RuntimeError("Scroll input failed")
            time.sleep(1)
            after_info,after = area.capture()
            area.image(after_info,after).save(out/f"{app}-{round_number+1}.png")
            if zlib.crc32(before)==zlib.crc32(after):
                raise RuntimeError(app+" did not visibly scroll")
            for other in documents:
                if not ping(other): raise RuntimeError(other+" stopped responding")
            results.append({"app":app,"round":round_number+1,"status":"passed"})
            print(json.dumps(results[-1]),flush=True)
    for app in documents:
        sp.run([str(root/"tests/check-render-faults.py"),str(out/(app+".log"))],check=True)
        assert "Unhandled exception" not in (out/(app+".log")).read_text(errors="replace")
    for app in documents:
        with (out/"quit.log").open("a") as log:
            sp.run([str(root/"tuxoffice"),"wine","C:/tuxoffice/office-probe.exe",app+"-quit"],
                   stdout=log,stderr=log,timeout=20,check=True)
finally:
    sp.run([str(root/"tuxoffice"),"kill"],check=True)
    (out/"summary.json").write_text(json.dumps(results,indent=2)+"\n")
