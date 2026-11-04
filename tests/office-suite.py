#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise Word, Excel and PowerPoint with fresh documents in a test prefix.

Build helpers first with tests/build-helpers.sh. Usage: office-suite.py OUTDIR [APP...]
The suite also checks the saved Office packages, extracted PDF text and exit.
"""
import json
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import time
from zipfile import ZipFile
import xml.etree.ElementTree as ET
from embedded_workbook import chart_workbook

root = Path(__file__).resolve().parent.parent
os.umask(0o077)
out = Path(sys.argv[1]).resolve()
prefix = os.environ.get("TUXOFFICE_PREFIX", "")
if not prefix.endswith("/home/prefix"):
    sys.exit("Functional tests require an isolated Office prefix")
out.mkdir(parents=True, exist_ok=True)
executables = {"word":b"WINWORD.EXE", "excel":b"EXCEL.EXE", "powerpoint":b"POWERPNT.EXE"}


def app_processes(app):
    pids = []
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            command = (process / "cmdline").read_bytes()
            environment = (process / "environ").read_bytes().split(b"\0")
            if command.startswith(b"C:") and executables[app] in command.upper() and ("WINEPREFIX="+prefix).encode() in environment:
                pids.append(int(process.name))
        except OSError:
            pass
    return pids


def verify(app, directory):
    stem, extension, marker = {
        "word":("word-document", "docx", "Verified table cell"),
        "excel":("excel-scatter", "xlsx", "TuxOffice scatter regression"),
        "powerpoint":("powerpoint-slides", "pptx", "TuxOffice slide 5"),
    }[app]
    with ZipFile(directory / f"{stem}.{extension}") as archive:
        if app == "word":
            document = ET.fromstring(archive.read("word/document.xml"))
            assert marker in ''.join(document.itertext()), "Saved table text is missing"
            assert "Tracked insertion verified." in ''.join(document.itertext()), "Accepted insertion is missing"
            assert "word/comments.xml" in archive.namelist(), "Saved comment is missing"
            assert any(n.startswith("word/header") for n in archive.namelist()), "Saved header is missing"
        elif app == "excel":
            charts = [n for n in archive.namelist() if re.fullmatch(r"xl/charts/chart\d+\.xml", n)]
            assert len(charts) == 1 and b"scatterChart" in archive.read(charts[0]), "Saved scatter chart is missing"
            assert b"conditionalFormatting" in archive.read("xl/worksheets/sheet1.xml"), "Saved conditional formatting is missing"
        else:
            slides = [n for n in archive.namelist() if re.fullmatch(r"ppt/slides/slide\d+\.xml", n)]
            assert len(slides) == 5, "Saved slide count differs"
            assert marker in ''.join(ET.fromstring(archive.read("ppt/slides/slide5.xml")).itertext()), "Last slide text is missing"
            assert "Verified slide table cell" in ''.join(ET.fromstring(archive.read("ppt/slides/slide5.xml")).itertext()), "Saved table cell is missing"
            charts = [n for n in archive.namelist() if re.fullmatch(r"ppt/charts/chart\d+\.xml", n)]
            assert len(charts) == 1 and b"scatterChart" in archive.read(charts[0]), "Saved presentation scatter is missing"
            with chart_workbook(archive) as embedded:
                sheet = ET.fromstring(embedded.read("xl/worksheets/sheet1.xml"))
                ns = {"x":"http://schemas.openxmlformats.org/spreadsheetml/2006/main"}
                value = sheet.find(".//x:c[@r='B2']/x:v", ns)
                assert value is not None and value.text == "37", "Edited embedded chart data was not saved"
    text_path = directory / "pdf-text.txt"
    sp.run(["pdftotext", str(directory / f"{stem}.pdf"), str(text_path)], check=True)
    text = text_path.read_text()
    assert marker in text, "Exported PDF does not contain expected text"
    if app == "word":
        assert "TuxOffice regression header" in text, "Exported header is missing"
    info = sp.run(["pdfinfo", str(directory / f"{stem}.pdf")], capture_output=True, text=True, check=True).stdout
    pages = int(re.search(r"^Pages:\s+(\d+)", info, re.M)[1])
    assert pages >= 1 and (app != "powerpoint" or pages == 5), "Unexpected PDF page count"
    return pages


results = []
try:
    for app in sys.argv[2:] or executables:
        directory = out / app
        directory.mkdir(exist_ok=True)
        result = {"app":app, "status":"failed"}
        try:
            sp.run([str(root / "tuxoffice"), "kill"], check=True)
            env = os.environ.copy()
            env["WINEDEBUG"] = os.environ.get("SUITE_WINEDEBUG", "-all,err+all,warn+seh")
            with (directory / "actions.log").open("w") as log:
                process = sp.run([str(root / "tuxoffice"), "wine", "C:/tuxoffice/office-functional.exe", app,
                                  "Z:" + str(directory).replace("/", "\\")], stdout=log, stderr=log,
                                 timeout=180, env=env)
            actions = (directory / "actions.log").read_text(errors="replace")
            passed = re.search(r"PASS app="+app+r" checks=(\d+) elapsed_ms=(\d+)", actions)
            assert process.returncode == 0 and passed, f"Functional helper exited {process.returncode}"
            assert "Unhandled exception" not in actions, "Unhandled exception recorded"
            sp.run([str(root/"tests/check-render-faults.py"),str(directory/"actions.log")],check=True)
            pages = verify(app, directory)
            deadline = time.monotonic() + 15
            while app_processes(app) and time.monotonic() < deadline:
                time.sleep(.25)
            assert not app_processes(app), "Office process remains after Quit and reference cleanup"
            result.update(status="passed", checks=int(passed[1]), elapsed_ms=int(passed[2]), pdf_pages=pages)
        except Exception as error:
            result["error"] = str(error)
        results.append(result)
        print(json.dumps(result), flush=True)
finally:
    sp.run([str(root / "tuxoffice"), "kill"], check=True)
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
sys.exit(0 if len(results) == 3 and all(r["status"] == "passed" for r in results) else 1)
