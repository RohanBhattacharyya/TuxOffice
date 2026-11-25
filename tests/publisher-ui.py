#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise Publisher typing and native picture insertion in an isolated session.

Usage: isolated-run.py SESSION python3 tests/publisher-ui.py OUTDIR [ROUNDS] [ribbon|inline]
Requires an editing-enabled Publisher installation, libei and the usual test helpers.
Checks portal ownership/activation, cancellation, insertion, save/reopen and PDF pixels.
The default exercises the actual Pictures ribbon button, including its source dialog.
The inline command separately tests the native chooser while that source dialog is broken.
"""
import json
import os
from pathlib import Path
import subprocess as sp
import sys
import tempfile
import time

from PIL import Image, ImageDraw

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1]).resolve()
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
mode = sys.argv[3] if len(sys.argv) > 3 else "ribbon"
assert mode in ("ribbon", "inline")
prefix = Path(os.environ.get("TUXOFFICE_PREFIX", ""))
assert str(prefix).endswith("/home/prefix"), "Use an isolated Office prefix"
assert 1 <= rounds <= 20
out.mkdir(parents=True, exist_ok=True)
work = prefix / "drive_c/tuxoffice/publisher-ui"
work.mkdir(parents=True, exist_ok=True)
progress = work / "progress.txt"
progress.unlink(missing_ok=True)
for artifact in ("publisher-ui.pub", "publisher-ui.pdf"):
    (work / artifact).unlink(missing_ok=True)
(work / "typed").unlink(missing_ok=True)
for marker in work.glob("stage-*"):
    marker.unlink()
image_path = work / "picture.png"
fixture = Image.new("RGB", (128, 96), (220, 20, 40))
ImageDraw.Draw(fixture).rectangle((64, 0, 127, 95), fill=(20, 60, 220))
fixture.save(image_path)
eis = out / "eis-input"
flags = sp.check_output(["pkg-config", "--cflags", "--libs", "libei-1.0", "libsystemd"], text=True).split()
sp.run(["cc", "-O2", "-o", str(eis), str(root / "tests/eis-input.c"), *flags], check=True)

script = r'''
On Error Resume Next
Set fso=CreateObject("Scripting.FileSystemObject")
base="C:\tuxoffice\publisher-ui"
Sub Note(s)
  Set f=fso.OpenTextFile(base & "\progress.txt",8,True):f.WriteLine s:f.Close
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
Sub Choose(stage)
  If INLINE Then box.TextFrame.TextRange.Select
  Note stage
  If INLINE Then a.CommandBars.ExecuteMso "PictureInsertFromFile"
  Do Until fso.FileExists(base & "\stage-" & Replace(stage," ","-")):WScript.Sleep 100:Loop
End Sub
Set a=CreateObject("Publisher.Application")
Set d=a.NewDocument()
a.ActiveWindow.Visible=True
Set box=d.Pages(1).Shapes.AddTextbox(1,72,72,400,80)
box.TextFrame.TextRange.Text="Replace this text"
box.TextFrame.TextRange.Select
Note "type"
Do Until fso.FileExists(base & "\typed"):WScript.Sleep 100:Loop
Check "interactive textbox", InStr(box.TextFrame.TextRange.Text,"Publisher interactive typing")>0
For i=1 To ROUNDS
  d.Selection.Unselect
  WScript.Sleep 500
  before=d.Pages(1).Shapes.Count
  Choose "cancel " & i
  Check "picture cancel " & i, d.Pages(1).Shapes.Count=before
  Choose "insert " & i
  Check "picture insert " & i, d.Pages(1).Shapes.Count=before+1
  Set picture=d.Pages(1).Shapes(d.Pages(1).Shapes.Count)
  picture.Name="UI picture " & i
  picture.Width=96:picture.Height=72
  picture.Left=72+((i-1) Mod 4)*110:picture.Top=200+Int((i-1)/4)*90
Next
d.SaveAs base & "\publisher-ui.pub"
Check "save", fso.FileExists(base & "\publisher-ui.pub")
Set picture=Nothing:Set box=Nothing
d.Close:Set d=Nothing
Set d=a.Open(base & "\publisher-ui.pub")
For i=1 To ROUNDS
  Set picture=d.Pages(1).Shapes("UI picture " & i)
  Check "reopen picture " & i, picture.Width=96
Next
d.ExportAsFixedFormat 2,base & "\publisher-ui.pdf"
Check "PDF", fso.FileExists(base & "\publisher-ui.pdf")
Note "done"
'''.replace("ROUNDS", str(rounds)).replace("INLINE", "True" if mode == "inline" else "False")
(work / "run.vbs").write_text(script)
results = []


def wine(*args, **kwargs):
    with tempfile.TemporaryFile(mode="w+") as output:
        result = sp.run([str(root / "tuxoffice"), "wine", *args], stdout=output,
                        stderr=output, text=True, errors="replace", timeout=20, **kwargs)
        output.seek(0)
        result.stdout = output.read()
        result.stderr = ""
    return result


def kwin(code):
    with tempfile.NamedTemporaryFile(mode="w", suffix=".js") as f:
        f.write(code)
        f.flush()
        result = sp.run([str(root / "tests/kwin-run.sh"), f.name], capture_output=True,
                        text=True, timeout=15, check=True)
    return [json.loads(line.split(" ", 1)[1]) for line in result.stdout.splitlines()
            if line.startswith("TUXWINDOW ")]


def windows():
    return kwin('for(const w of workspace.windowList()) print("TUXWINDOW",JSON.stringify('
                '{id:String(w.internalId),caption:w.caption,app:w.resourceClass,active:w===workspace.activeWindow,'
                'modal:w.modal,transient:w.transient,parent:w.transientFor?w.transientFor.resourceClass:null}));')


def native_input(*items):
    commands = sp.check_output([sys.executable, str(root / "tests/eis-type.py"), *items])
    sp.run([str(eis)], input=commands, timeout=20, check=True, stdout=sp.DEVNULL)


sp.run([str(root / "tuxoffice"), "kill"], check=True)
env = dict(os.environ, WINEDEBUG=os.environ.get("PUBLISHER_UI_WINEDEBUG", "-all,err+all,warn+seh"),
           DXVK_LOG_LEVEL="error")
log = (out / "publisher.log").open("w")
process = sp.Popen([str(root / "tuxoffice"), "wine", "cscript", "//nologo",
                   r"C:\tuxoffice\publisher-ui\run.vbs"], stdout=log, stderr=log, env=env)
handled = set()
triggered = set()
deadline = time.monotonic() + 180 + rounds * 15
try:
    while True:
        lines = progress.read_text().splitlines() if progress.exists() else []
        assert time.monotonic() < deadline, "Publisher UI test timed out"
        assert not any(line.startswith("FAIL ") for line in lines), "\n".join(lines)
        stage = next((line for line in reversed(lines) if not line.startswith("PASS ")), "")
        if stage == "done":
            break
        if stage and stage not in handled:
            if stage == "type":
                sp.run([sys.executable, str(root / "tests/activate-office.py"), "publisher"], check=True)
                result = wine("C:/tuxoffice/typer.exe", "--app", "publisher", "type",
                              "Publisher interactive typing", "10")
                assert result.returncode == 0, result.stdout + result.stderr
                (work / "typed").touch()
            elif stage.startswith(("cancel ", "insert ")):
                if mode == "ribbon" and stage not in triggered:
                    sp.run([sys.executable, str(root / "tests/activate-office.py"), "publisher"], check=True)
                    for name, role in (("Home", "37"), ("Pictures...", "43")):
                        result = wine("C:/tuxoffice/winctl.exe", "acc-click", "MSWinPub", name, role)
                        assert result.returncode == 0, result.stdout + result.stderr
                        time.sleep(.2)
                    triggered.add(stage)
                chooser = next((w for w in windows() if w["app"] == "org.freedesktop.impl.portal.desktop.kde"), None)
                if not chooser:
                    time.sleep(.2)
                    continue
                assert chooser["modal"] and chooser["transient"] and chooser["parent"] == "mspub.exe", chooser
                kwin("const w=workspace.windowList().find(w=>w.resourceClass==='mspub.exe');"
                     "if(w){workspace.raiseWindow(w);workspace.activeWindow=w;}")
                assert any(w["active"] and w["parent"] == "mspub.exe" for w in windows()), "Chooser lost activation"
                if stage.startswith("cancel "):
                    native_input("{escape}")
                else:
                    native_input("{ctrl+a}", str(image_path))
                    assert any(w["id"] == chooser["id"] and w["active"] for w in windows()), "Chooser closed while typing"
                    native_input("{enter}")
                # The portal's old Wayland window can outlive its response.
                # Do not mistake it for the next request and type into Office.
                closed = time.monotonic() + 10
                while any(w["id"] == chooser["id"] for w in windows()):
                    assert time.monotonic() < closed, "Native chooser did not close"
                    time.sleep(.1)
                (work / ("stage-" + stage.replace(" ", "-"))).touch()
            else:
                time.sleep(.2)
                continue
            handled.add(stage)
            results.append(stage)
            print(stage, flush=True)
        time.sleep(.2)
    expected = {"interactive textbox", "save", "PDF"}
    expected.update(f"{name} {i}" for name in ("picture cancel", "picture insert", "reopen picture")
                    for i in range(1, rounds + 1))
    passed = {line.removeprefix("PASS ") for line in lines if line.startswith("PASS ")}
    # VBScript's On Error Resume Next can skip a whole Check call if a COM
    # expression fails while its arguments are evaluated. Require every check.
    assert passed == expected, f"Missing checks: {sorted(expected - passed)}; unexpected: {sorted(passed - expected)}"
    sp.run(["pdftoppm", "-singlefile", "-scale-to", "1200", "-png", str(work / "publisher-ui.pdf"),
            str(out / "publisher-pdf")], check=True, stdout=sp.DEVNULL, stderr=sp.DEVNULL)
    data = Image.open(out / "publisher-pdf.png").convert("RGB").tobytes()
    red = blue = 0
    for r, g, b in zip(data[0::3], data[1::3], data[2::3]):
        red += r > 150 and g < 80 and b < 100
        blue += b > 150 and r < 80 and g < 100
    assert red > rounds * 500 and blue > rounds * 500, "Inserted pictures are missing from PDF"
    faults = (out / "publisher.log").read_text(errors="replace")
    assert "Unhandled exception" not in faults and "page fault" not in faults, "Publisher faulted"
    summary = {"status": "passed", "mode": mode, "rounds": rounds, "checks": sum(l.startswith("PASS ") for l in lines),
               "native_dialogs": len(handled) - 1, "red_pixels": red, "blue_pixels": blue}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))
except Exception as error:
    (out / "summary.json").write_text(json.dumps({"status": "failed", "mode": mode,
        "error": str(error), "progress": progress.read_text().splitlines() if progress.exists() else []}, indent=2) + "\n")
    raise
finally:
    # Close through the window: the separately tracked COM Quit hang must not
    # prevent cleanup of this test's disposable document.
    try:
        wine("C:/tuxoffice/winctl.exe", "close", "WebView2Dialog_v2")
        wine("C:/tuxoffice/winctl.exe", "close", "MSWinPub")
    except sp.TimeoutExpired:
        pass
    try:
        process.wait(timeout=8)
    except sp.TimeoutExpired:
        process.kill()
    sp.run([str(root / "tuxoffice"), "kill"], check=False)
    log.close()
