#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Complete the real Access Labels wizard, export through KDE, and reopen.

Usage: isolated-run.py SESSION python3 tests/access-labels.py OUTDIR [ROUNDS]
Requires editing-enabled Access, winctl/typer, KWin and libei. No wizard callback
or Office code is replaced. Verification uses the application's object model.
"""
import json
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 1
prefix = Path(os.environ.get("TUXOFFICE_PREFIX", ""))
assert str(prefix).endswith("/home/prefix"), "Use an isolated Office prefix"
assert 1 <= rounds <= 20
out.mkdir(parents=True, exist_ok=True)
work = prefix / "drive_c/tuxoffice/access-labels"
work.mkdir(exist_ok=True)
for path in work.iterdir():
    if path.is_file():
        path.unlink()
progress = work / "progress.txt"
script = r'''
Option Explicit
On Error Resume Next
Dim fs,a,db,n,i,p,f,c,r,name,checked
Set fs=CreateObject("Scripting.FileSystemObject")
Sub Note(s)
 Dim file:Set file=fs.OpenTextFile("C:\tuxoffice\access-labels\progress.txt",8,True)
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
 Do Until fs.FileExists("C:\tuxoffice\access-labels\" & Replace(s," ","-") & ".done")
  WScript.Sleep 100
 Loop
End Sub
Set a=CreateObject("Access.Application")
a.Visible=True
For n=1 To ROUNDS
 p="C:\tuxoffice\access-labels\labels-" & n & ".accdb"
 a.NewCurrentDatabase p
 Set db=a.CurrentDb
 db.Execute "CREATE TABLE Items (Id AUTOINCREMENT PRIMARY KEY, ItemName TEXT(50), Qty LONG)",128
 For i=1 To 25
  db.Execute "INSERT INTO Items (ItemName, Qty) VALUES ('Item " & i & "', " & i & ")",128
 Next
 a.DoCmd.OpenTable "Items"
 a.DoCmd.SelectObject 0,"Items",True
 Check "fixture " & n,True
 Stage "open " & n
 Set f=a.Forms("mlbl_frmMain").Controls("PageSubForm").Form
 checked=Empty:checked=(f.Controls("lstSizes").ListCount>0):Check "sizes " & n,checked
 Stage "fields " & n
 Set f=a.Forms("mlbl_frmMain").Controls("PageSubForm").Form
 Set c=f.Controls("lstSource")
 checked=Empty:checked=(c.ListCount=3):Check "field count " & n,checked
 checked=Empty:checked=(c.Column(0,0)="Id" And c.Column(0,1)="ItemName" And c.Column(0,2)="Qty")
 Check "field names " & n,checked
 Stage "prototype " & n
 checked=Empty:checked=(f.Controls("tbl1").Value="{ItemName}"):Check "prototype " & n,checked
 Set c=Nothing:Set f=Nothing
 Stage "report " & n
 checked=Empty:checked=(a.Reports.Count=1):Check "report open " & n,checked
 Set r=a.Reports(0):name=r.Name
 checked=Empty:checked=(InStr(r.RecordSource,"Items")>0):Check "report source " & n,checked
 Stage "export " & n
 Set r=Nothing:Set db=Nothing
 a.DoCmd.Close 3,name,1
 a.CloseCurrentDatabase
 a.OpenCurrentDatabase p
 checked=Empty:checked=(a.CurrentProject.AllReports.Count=1):Check "saved report " & n,checked
 a.DoCmd.OpenReport name,2
 checked=Empty:checked=(a.Reports.Count=1):Check "reopen preview " & n,checked
 a.DoCmd.Close 3,name,2
 a.CloseCurrentDatabase
Next
a.Quit 2
Check "quit",True
Note "DONE"
'''.replace("ROUNDS", str(rounds))
(work / "labels.vbs").write_text(script)
eis = out / "eis-input"
flags = sp.check_output(["pkg-config", "--cflags", "--libs", "libei-1.0", "libsystemd"], text=True).split()
sp.run(["cc", "-O2", "-o", str(eis), str(root / "tests/eis-input.c"), *flags], check=True)
env = dict(os.environ, WINEDEBUG="-all,err+all", DXVK_LOG_LEVEL="error")


def wine(*args):
    return sp.run([str(root / "tuxoffice"), "wine", *args], env=env, check=True,
                  capture_output=True, text=True, timeout=60).stdout


def wait_for(test, description, seconds=30):
    deadline = time.monotonic() + seconds
    while not test():
        assert process.poll() is None, "Access fixture exited: " + description
        assert time.monotonic() < deadline, "Timed out: " + description
        time.sleep(.15)


def stage(name):
    wait_for(lambda: progress.exists() and f"STAGE {name}\n" in progress.read_text(), name)
    assert "FAIL " not in progress.read_text(), progress.read_text()


def done(name):
    (work / (name.replace(" ", "-") + ".done")).touch()


def click(x, y):
    wine("C:/tuxoffice/winctl.exe", "click-relative", "Label Wizard", str(x), str(y), "705", "441")
    time.sleep(.3)


def portal():
    with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as source:
        source.write("for(const w of workspace.windowList()) if(w.resourceClass==="
                     "'org.freedesktop.impl.portal.desktop.kde') "
                     "print('TUXPICKER',w===workspace.activeWindow);")
        source.flush()
        return sp.check_output([str(root / "tests/kwin-run.sh"), source.name], text=True, timeout=15)


results = []
process = None
try:
    sp.run([str(root / "tuxoffice"), "kill"], check=True)
    with (out / "access.log").open("w") as log:
        process = sp.Popen([str(root / "tuxoffice"), "wine", "cscript", "//nologo",
                            "C:/tuxoffice/access-labels/labels.vbs"], env=env, stdout=log, stderr=log)
        for n in range(1, rounds + 1):
            stage(f"open {n}")
            sp.run([sys.executable, str(root / "tests/activate-office.py"), "access"], check=True)
            wine("C:/tuxoffice/winctl.exe", "acc-click", "OMain", "Create", "37")
            wine("C:/tuxoffice/winctl.exe", "acc-click", "OMain", "Labels", "43")
            done(f"open {n}")
            stage(f"fields {n}")
            click(537, 412)  # sizes -> font
            click(537, 412)  # font -> available fields
            done(f"fields {n}")
            stage(f"prototype {n}")
            sp.run([sys.executable, str(root / "tests/capture-word.py"),
                    str(out / f"fields-{n}.png"), "access"], check=True, capture_output=True)
            click(70, 185)  # ItemName
            click(278, 221)  # add it to the prototype
            done(f"prototype {n}")
            stage(f"report {n}")
            click(537, 412)  # sort
            click(216, 226)  # Qty
            click(444, 201)  # ascending sort
            click(537, 412)  # report name
            click(641, 412)  # finish with default print-preview option
            wait_for(lambda: "OFormPopup" not in wine("C:/tuxoffice/winctl.exe", "list"), "wizard closes")
            done(f"report {n}")
            stage(f"export {n}")
            sp.run([sys.executable, str(root / "tests/capture-word.py"),
                    str(out / f"preview-{n}.png"), "access"], check=True, capture_output=True)
            wine("C:/tuxoffice/winctl.exe", "acc-click", "OMain", "PDF or XPS", "43")
            wait_for(lambda: "true" in portal(), "native PDF save dialog")
            pdf = work / f"labels-{n}.pdf"
            commands = sp.check_output([sys.executable, str(root / "tests/eis-type.py"),
                                        "{ctrl+a}", str(pdf), "{enter}"])
            sp.run([str(eis)], input=commands, check=True, timeout=20, stdout=sp.DEVNULL)
            wait_for(lambda: pdf.exists() and pdf.stat().st_size > 1000, "PDF export")
            text = sp.check_output(["pdftotext", "-layout", str(pdf), "-"], text=True)
            numbers = [int(x) for x in re.findall(r"\bItem (\d+)\b", text)]
            assert numbers == list(range(1, 26)), numbers
            wait_for(lambda: "Export - PDF" in wine("C:/tuxoffice/winctl.exe", "list"),
                     "save export steps dialog")
            wine("C:/tuxoffice/winctl.exe", "acc-click", "Export - PDF", "Close", "43")
            done(f"export {n}")
            results.append({"round": n, "native_save_dialog": True, "pdf_items": len(numbers), "sorted": True})
            print(json.dumps(results[-1]), flush=True)
        wait_for(lambda: "DONE\n" in progress.read_text(), "saved report verification")
        process.wait(timeout=20)
        lines = progress.read_text().splitlines()
        assert process.returncode == 0 and not any(line.startswith("FAIL ") for line in lines), lines
        assert sum(line.startswith("PASS ") for line in lines) == 9 * rounds + 1, lines
        faults = [line for line in (out / "access.log").read_text(errors="replace").splitlines()
                  if "Unhandled exception" in line or "page fault" in line]
        assert not faults, faults
        summary = {"status": "passed", "rounds": rounds, "checks": 9 * rounds + 1, "results": results}
        (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary), flush=True)
except Exception as error:
    (out / "summary.json").write_text(json.dumps({"status": "failed", "error": str(error), "results": results}, indent=2) + "\n")
    raise
finally:
    sp.run([str(root / "tuxoffice"), "kill"], check=False)
    if process is not None:
        process.wait(timeout=15)
