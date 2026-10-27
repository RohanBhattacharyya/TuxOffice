#!/usr/bin/env python3
"""Edit PowerPoint's chart in full Excel and close it through the desktop UI.

Unlike the embedded data grid, full Excel is opened asynchronously in a new
process. Wait for its real document window before sending input; do not close
the old workbook COM proxy during that handoff.
"""
import json
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from zipfile import ZipFile

from office_windows import prefix_pids
from embedded_workbook import chart_workbook

root = Path(__file__).resolve().parent.parent
os.umask(0o077)
out = Path(sys.argv[1]).resolve()
if not os.environ.get("TUXOFFICE_PREFIX", "").endswith("/home/prefix"):
    sys.exit("This test requires the disposable Office prefix")
out.mkdir(parents=True, exist_ok=True)
trigger = out/"editor-closed"
trigger.unlink(missing_ok=True)
summary = {"status":"failed", "scenario":"PowerPoint chart edited in full Excel"}


def typer(*args):
    with tempfile.TemporaryFile(mode="w+") as output:
        result = sp.run([str(root/"tuxoffice"), "wine", "C:/tuxoffice/typer.exe", "--app", "excel", *map(str,args)],
                        stdout=output, stderr=output, timeout=12)
        output.seek(0)
        text = output.read()
    with (out/"input.log").open("a") as log:
        log.write(f"{args} exit={result.returncode}\n{text}")
    return result.returncode, text


def require_input(*args):
    assert typer(*args)[0] == 0, "Excel did not accept input: "+str(args)


client = None
try:
    sp.run([str(root/"tuxoffice"), "kill"], check=True)
    env = dict(os.environ, WINEDEBUG="-all,err+all,warn+seh", TUXOFFICE_CHART_FULL_EXCEL="1",
               TUXOFFICE_CHART_CLOSE_TRIGGER="Z:"+str(trigger).replace("/", "\\"))
    with (out/"actions.log").open("w") as log:
        client = sp.Popen([str(root/"tuxoffice"), "wine", "C:/tuxoffice/office-functional.exe", "powerpoint",
                           "Z:"+str(out).replace("/", "\\")], env=env, stdout=log, stderr=log)
    deadline = time.monotonic()+80
    while "READY chart editor UI test" not in (out/"actions.log").read_text(errors="replace"):
        assert client.poll() is None, "PowerPoint helper exited before editing"
        if time.monotonic()>deadline: raise TimeoutError("Chart editor did not become ready")
        time.sleep(.2)
    title = "Chart in Microsoft PowerPoint"
    deadline = time.monotonic()+25
    while True:
        activation = sp.run([sys.executable,str(root/"tests/activate-office.py"),"excel",title],
                            stdout=sp.DEVNULL,stderr=sp.DEVNULL)
        if activation.returncode == 0 and title in typer("foreground")[1] and typer("ping",700)[0] == 0:
            break
        if time.monotonic()>deadline: raise TimeoutError("Full Excel document window did not appear")
        time.sleep(.3)
    require_input("raise")
    require_input("chord","0x11","0x47")  # Ctrl+G: Go To B2.
    time.sleep(.3)
    require_input("type","B2",35)
    require_input("key","0x0d")
    time.sleep(.3)
    require_input("type","74",35)
    require_input("key","0x0d")
    time.sleep(.5)
    sp.run([str(root/"tests/capture-word.py"),str(out/"excel-edited.png"),"excel","--active"],check=True)
    require_input("chord","0x12","0x73")  # Alt+F4: normal editor close.
    deadline = time.monotonic()+15
    while time.monotonic()<deadline:
        status, response = typer("ping",500)
        if response.startswith("noword"): break
        if response.startswith("modal"): require_input("key","0x0d")
        time.sleep(.3)
    else: raise TimeoutError("Excel editor did not close")
    trigger.touch()
    assert client.wait(timeout=60) == 0, "PowerPoint helper failed after the editor closed"
    actions = (out/"actions.log").read_text(errors="replace")
    assert "PASS app=powerpoint" in actions, "PowerPoint did not complete its round trip"
    assert not re.search(r"Unhandled (?:exception|page fault)", actions), "An Office process crashed"
    sp.run([str(root/"tests/check-render-faults.py"),str(out/"actions.log")],check=True)
    with ZipFile(out/"powerpoint-slides.pptx") as presentation:
        with chart_workbook(presentation) as workbook:
            sheet = ET.fromstring(workbook.read("xl/worksheets/sheet1.xml"))
            value = sheet.find(".//{*}c[@r='B2']/{*}v")
            assert value is not None and value.text == "74", "UI edit did not survive presentation save"
    deadline = time.monotonic()+10
    while (prefix_pids("excel") or prefix_pids("powerpoint")) and time.monotonic()<deadline:
        time.sleep(.2)
    assert not prefix_pids("excel") and not prefix_pids("powerpoint"), "An Office app remained after Quit"
    summary.update(status="passed", edited_cell="B2", saved_value=74, editor="full Excel",
                   close="Alt+F4", office_processes_after_quit=0)
except Exception as error:
    summary["error"] = str(error)
finally:
    trigger.touch()
    if client is not None and client.poll() is None:
        client.terminate()
    sp.run([str(root/"tuxoffice"), "kill"], check=True)
    (out/"summary.json").write_text(json.dumps(summary,indent=2)+"\n")
print(json.dumps(summary))
sys.exit(0 if summary["status"]=="passed" else 1)
