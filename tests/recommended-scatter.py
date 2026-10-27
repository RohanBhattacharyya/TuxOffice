#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise Excel's Recommended Charts UI in the isolated Office prefix."""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time
from zipfile import ZipFile, ZIP_DEFLATED

root = Path(__file__).resolve().parent.parent
os.umask(0o077)
out = Path(sys.argv[1]).resolve()
prefix = os.environ.get("TUXOFFICE_PREFIX", "")
if not prefix.endswith("/home/prefix"):
    sys.exit("This test requires an isolated Office prefix")
out.mkdir(parents=True, exist_ok=True)
fixture = out / "recommended-scatter-data.xlsx"
files = {
    "[Content_Types].xml": '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>',
    "_rels/.rels": '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>',
    "xl/workbook.xml": '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Scatter data" sheetId="1" r:id="rId1"/></sheets></workbook>',
    "xl/_rels/workbook.xml.rels": '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>',
}
rows = ['<row r="1"><c r="A1" t="inlineStr"><is><t>IE Energy</t></is></c><c r="B1" t="inlineStr"><is><t>Numbers</t></is></c></row>']
rows += [f'<row r="{i+1}"><c r="A{i+1}"><v>{100*i}</v></c><c r="B{i+1}"><v>{i}</v></c></row>' for i in range(1, 6)]
files["xl/worksheets/sheet1.xml"] = '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetViews><sheetView workbookViewId="0"><selection activeCell="A1" sqref="A1:B6"/></sheetView></sheetViews><sheetData>' + ''.join(rows) + '</sheetData></worksheet>'
with ZipFile(fixture, "w", ZIP_DEFLATED) as archive:
    for name, content in files.items():
        archive.writestr(name, content)


def wine(helper, *args, timeout=10):
    # Wine services can inherit the first client's stdout. A pipe would wait
    # for their exit after this short-lived helper has already completed.
    with tempfile.TemporaryFile(mode="w+") as output:
        result = sp.run([str(root / "tuxoffice"), "wine", f"C:/tuxoffice/{helper}.exe", *map(str, args)],
                        stdout=output, stderr=output, text=True, timeout=timeout)
        output.seek(0)
        result.stdout = output.read()
        result.stderr = ""
    with (out / "actions.log").open("a") as log:
        log.write(f"{helper} {args}: status={result.returncode}\n{result.stdout}{result.stderr}")
    return result


def typer(*args, **kwargs):
    return wine("typer", "--app", "excel", *args, **kwargs)


def ready(timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if typer("ping", 700, timeout=30).stdout.startswith("ok"):
            return
        time.sleep(.3)
    raise RuntimeError("Excel UI did not become responsive")


trigger = out / "trigger"
launcher = None
verdict = {"scenario": "recommended-scatter", "status": "failed"}
try:
    sp.run([str(root / "tuxoffice"), "kill"], check=True)
    env = os.environ.copy()
    env["TUX_GDB_APP"] = "excel"
    env["WINEDEBUG"] = "-all,err+all,warn+seh"
    launcher = sp.Popen([str(root / "tests/gdb-word.sh"), str(trigger), str(out / "gdb.txt"), str(fixture)], env=env)
    ready()
    zoom = wine("office-probe", "excel-zoom", 179, timeout=30)
    if zoom.returncode:
        raise RuntimeError("Excel zoom automation failed")
    sp.run([sys.executable,str(root / "tests/activate-office.py"),"excel","recommended-scatter-data"],check=True)
    for vk in (0x12, 0x4e, 0x52):
        if typer("key", hex(vk)).returncode:
            raise RuntimeError("Excel lost focus during input")
        time.sleep(.15)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if typer("ping", 700).stdout.startswith("modal"):
            break
        time.sleep(.2)
    else:
        raise RuntimeError("Recommended Charts dialog did not open")
    time.sleep(1)
    if typer("dialog-click", "Insert Chart", 100, 330).returncode:
        raise RuntimeError("Scatter recommendation selection failed")
    time.sleep(.5)
    sp.run([str(root / "tests/capture-word.py"), str(out / "scatter-preview.png"), "excel", "--active"], check=True)
    if typer("key", "0x0d").returncode:
        raise RuntimeError("Chart import did not receive Enter")
    time.sleep(2)
    ready(15)
    saved = out / "recommended-result.xlsx"
    result = wine("office-probe", "excel-chart-check", "Z:" + str(saved).replace("/", "\\"), timeout=20)
    if result.returncode:
        raise RuntimeError("Inserted chart could not be inspected and saved")
    with ZipFile(saved) as archive:
        charts = [name for name in archive.namelist() if name.startswith("xl/charts/chart") and name.endswith(".xml")]
        if len(charts) != 1 or b"scatterChart" not in archive.read(charts[0]):
            raise RuntimeError("Saved workbook does not contain exactly one scatter chart")
    # The original failure occurs after insertion while the chart is visible.
    for i in range(20):
        response = typer("ping", 700)
        if not response.stdout.startswith("ok"):
            raise RuntimeError(f"Excel became unresponsive after insertion: {response.stdout.strip()}")
        time.sleep(.25)
    sp.run([str(root / "tests/capture-word.py"), str(out / "inserted-chart.png"), "excel"], check=True)
    verdict.update(status="passed", chart_type=result.stdout.strip(), workbook=str(saved))
except Exception as error:
    verdict["error"] = str(error)
finally:
    trigger.touch()
    if launcher:
        try:
            launcher.wait(timeout=20)
        except sp.TimeoutExpired:
            verdict.update(status="failed", debugger_error="Stack capture did not complete")
    if verdict["status"] == "passed":
        log=out/"gdb.txt.wine.log"
        scan=sp.run([str(root/"tests/check-render-faults.py"),str(log)])
        if scan.returncode or "Unhandled exception" in log.read_text(errors="replace"):
            verdict.update(status="failed",render_error="An Office rendering exception was recorded")
    if verdict["status"] == "passed":
        quit_result = wine("office-probe", "excel-quit", timeout=15)
        if quit_result.returncode:
            verdict.update(status="failed", quit_error="Excel did not quit cleanly")
    (out / "summary.json").write_text(json.dumps(verdict, indent=2) + "\n")
print(json.dumps(verdict), flush=True)
sys.exit(0 if verdict["status"] == "passed" else 1)
