#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Stress Access's actual Create ribbon and edit the generated forms.

Usage: isolated-run.py SESSION python3 tests/access-ui.py OUTDIR [ROUNDS]
Uses only a new database under C:/tuxoffice/access-ui. Requires an editing-enabled
Access installation, winctl.exe and typer.exe from tests/build-helpers.sh.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
prefix = Path(os.environ.get("TUXOFFICE_PREFIX", ""))
assert str(prefix).endswith("/home/prefix"), "Use an isolated prefix"
assert 1 <= rounds <= 20
out.mkdir(parents=True, exist_ok=True)
work = prefix / "drive_c/tuxoffice/access-ui"
work.mkdir(parents=True, exist_ok=True)
for item in work.iterdir():
    if item.is_file():
        item.unlink()
progress = work / "progress.txt"
script = r'''
Option Explicit
On Error Resume Next
Dim fso,a,db,f,r,forms,views,kind,i,j,checked,started,name,rs,reportName,generatedName
Set fso=CreateObject("Scripting.FileSystemObject")
Sub Note(s)
  Dim file:Set file=fso.OpenTextFile("C:\tuxoffice\access-ui\progress.txt",8,True)
  file.WriteLine s:file.Close
End Sub
Sub Check(s,ok)
  If Err.Number<>0 Then
    Note "FAIL " & s & " " & Hex(Err.Number) & " " & Err.Description:Err.Clear
  ElseIf Not ok Then
    Note "FAIL " & s
  Else
    Note "PASS " & s
  End If
End Sub
Sub Stage(s)
  Note "STAGE " & s
  Do Until fso.FileExists("C:\tuxoffice\access-ui\" & Replace(s," ","-") & ".done")
    WScript.Sleep 100
  Loop
End Sub
Set a=CreateObject("Access.Application")
a.Visible=True
a.NewCurrentDatabase "C:\tuxoffice\access-ui\stress.accdb"
Set db=a.CurrentDb
db.Execute "CREATE TABLE Items (Id AUTOINCREMENT PRIMARY KEY, ItemName TEXT(50), Qty LONG)",128
For i=1 To 25
  db.Execute "INSERT INTO Items (ItemName, Qty) VALUES ('Item " & i & "', " & i & ")",128
Next
checked=Empty:checked=(db.OpenRecordset("Items").RecordCount>0):Check "fixture",checked
forms=Array("Form","Multiple Items","Datasheet","Split Form")
views=Array(0,1,2,5)
For i=1 To ROUNDS
  For j=0 To UBound(forms)
    kind=forms(j)
    a.DoCmd.OpenTable "Items"
    Stage "create " & kind & " " & i
    started=Timer
    Do While a.Forms.Count=0 And Timer-started<15:WScript.Sleep 100:Loop
    Set f=Nothing:Set f=a.Screen.ActiveForm
    name="TuxForm" & i & "_" & j
    checked=Empty:checked=(a.Forms.Count=1):Check name & " created",checked
    checked=Empty:checked=(f.RecordSource="Items" And f.DefaultView=views(j)):Check name & " binding and view",checked
    generatedName=f.Name
    a.DoCmd.Save 2,f.Name
    a.DoCmd.Close 2,f.Name,1
    a.DoCmd.Rename name,2,generatedName
    Set f=Nothing
    a.DoCmd.Close 0,"Items",2
    a.DoCmd.OpenForm name
    Set f=a.Forms(name)
    Set rs=f.RecordsetClone:rs.MoveLast
    checked=Empty:checked=(rs.RecordCount=25):Check name & " records",checked
    rs.Close:Set rs=Nothing
    f.Controls("ItemName").SetFocus
    Stage "type " & kind & " " & i
    f.Dirty=False
    checked=Empty:checked=(f.Controls("ItemName").Value="Access UI " & i & " " & j):Check name & " typed",checked
    a.DoCmd.Close 2,name,1
    Set f=Nothing
    a.DoCmd.OpenForm name
    checked=Empty:checked=(a.Forms(name).Controls("ItemName").Value="Access UI " & i & " " & j):Check name & " reopen",checked
    a.DoCmd.Close 2,name,1
  Next
Next
a.DoCmd.OpenTable "Items"
Stage "create Report"
Set r=Nothing:Set r=a.Screen.ActiveReport
reportName=r.Name
checked=Empty:checked=(r.RecordSource="Items"):Check "report binding",checked
a.DoCmd.Save 3,reportName
a.DoCmd.Close 3,reportName,1
a.DoCmd.OutputTo 3,reportName,"PDF Format (*.pdf)","C:\tuxoffice\access-ui\report.pdf",False
checked=Empty:checked=fso.FileExists("C:\tuxoffice\access-ui\report.pdf"):Check "report PDF",checked
Set r=Nothing:Set db=Nothing
a.CloseCurrentDatabase
a.OpenCurrentDatabase "C:\tuxoffice\access-ui\stress.accdb"
checked=Empty:checked=(a.CurrentProject.AllForms.Count=4*ROUNDS):Check "reopen database forms",checked
a.CloseCurrentDatabase:a.Quit
Note "DONE"
'''.replace("ROUNDS", str(rounds))
(work / "run.vbs").write_text(script)


def wine(*args):
    with tempfile.TemporaryFile(mode="w+") as output:
        result = sp.run([str(root / "tuxoffice"), "wine", *args], stdout=output, stderr=output,
                        text=True, errors="replace", timeout=20)
        output.seek(0)
        result.stdout = output.read()
    assert result.returncode == 0, result.stdout
    return result.stdout


def click(window, name, role):
    wine("C:/tuxoffice/winctl.exe", "acc-click", window, name, str(role))
    time.sleep(.3)


sp.run([str(root / "tuxoffice"), "kill"], check=True)
log = (out / "access.log").open("w")
process = sp.Popen([str(root / "tuxoffice"), "wine", "cscript", "//nologo", r"C:\tuxoffice\access-ui\run.vbs"],
                   stdout=log, stderr=log, env=dict(os.environ, WINEDEBUG="-all,err+all,warn+seh", DXVK_LOG_LEVEL="error"))
handled = set()
summary = {"status": "interrupted"}
deadline = time.monotonic() + 180 + rounds * 90
try:
    while True:
        lines = progress.read_text().splitlines() if progress.exists() else []
        assert process.poll() is None or "DONE" in lines, "Access automation exited early"
        assert time.monotonic() < deadline, "Access UI test timed out"
        assert not any(line.startswith("FAIL ") for line in lines), "\n".join(lines)
        if "DONE" in lines:
            break
        stage = next((line[6:] for line in reversed(lines) if line.startswith("STAGE ")), "")
        if stage and stage not in handled:
            sp.run([sys.executable, str(root / "tests/activate-office.py"), "access"], check=True)
            if stage.startswith("create "):
                name = stage[7:] if stage == "create Report" else stage[7:].rsplit(" ", 1)[0]
                click("OMain", "Create", 37)
                if name in ("Multiple Items", "Datasheet", "Split Form"):
                    click("OMain", "More Forms", 56)
                    click("Net UI Tool Window", name, 12)
                else:
                    click("OMain", name, 43)
            else:
                name, iteration = stage[5:].rsplit(" ", 1)
                index = ("Form", "Multiple Items", "Datasheet", "Split Form").index(name)
                if iteration == "1":
                    sp.run([sys.executable, str(root / "tests/capture-word.py"),
                            str(out / (name.replace(" ", "-") + ".png")), "access"],
                           check=True, timeout=20, stdout=sp.DEVNULL)
                wine("C:/tuxoffice/typer.exe", "--app", "access", "type", f"Access UI {iteration} {index}", "10")
            (work / (stage.replace(" ", "-") + ".done")).touch()
            handled.add(stage)
            print(stage, flush=True)
        time.sleep(.2)
    process.wait(timeout=15)
    assert process.returncode == 0
    expected = {"fixture", "report binding", "report PDF", "reopen database forms"}
    expected.update(f"TuxForm{i}_{j} {check}" for i in range(1, rounds + 1) for j in range(4)
                    for check in ("created", "binding and view", "records", "typed", "reopen"))
    passed = {line[5:] for line in lines if line.startswith("PASS ")}
    assert passed == expected, f"Missing checks: {sorted(expected - passed)}"
    pdf = sp.check_output(["pdftotext", str(work / "report.pdf"), "-"], text=True)
    assert f"Access UI {rounds} 3" in pdf and "Item 25" in pdf, "Report data missing from PDF"
    faults = (out / "access.log").read_text(errors="replace")
    assert "Unhandled exception" not in faults and "page fault" not in faults, "Access faulted"
    summary = {"status": "passed", "rounds": rounds, "checks": len(passed), "stages": len(handled)}
    print(json.dumps(summary))
except Exception as error:
    summary = {"status": "failed", "error": str(error),
               "progress": progress.read_text().splitlines() if progress.exists() else []}
    raise
finally:
    sp.run([str(root / "tuxoffice"), "kill"], check=False)
    process.wait(timeout=10)
    log.close()
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
