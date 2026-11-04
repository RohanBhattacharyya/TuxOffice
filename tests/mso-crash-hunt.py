#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run every ribbon command of an Office app, one by one, looking for crashes and hangs.

Usage (inside tests/isolated-run.py, without a network, see below):
    mso-crash-hunt.py OUTDIR APP DOCUMENT CANDIDATES [START]

APP is word, excel, powerpoint, publisher, access or outlook. Outlook works in its inbox, and DOCUMENT
is ignored; back up its data files first, since its commands move, delete and change items. DOCUMENT is a Windows path to a document to
work in, reopened regularly so that one command's damage does not hide the rest. CANDIDATES is a
file of possible ribbon command names, one per line, or "controls" to go through the numeric
control ids instead, which reach many more commands. Access always uses RunCommand's numbers.
What the app accepts is kept in OUTDIR/commands.txt, and later runs reuse that. Build
dialog-closer.exe first and copy it to C:\\tuxoffice. Open and Save As go to the desktop file
chooser, which the helper cannot close, so they are reported as hangs with no fault and the main
window disabled.

Commands can do anything an app can, so run this with no network, on test files, in the isolated
prefix: unshare --user --map-current-user --net -- mso-crash-hunt.py ...

Each crash or hang is recorded in OUTDIR/findings.jsonl with its command, then the app is started
again from the next command. OUTDIR/run.log has each app's error output.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import time

root = Path(__file__).resolve().parent.parent
tux = str(root / "tuxoffice")
out = Path(sys.argv[1]).resolve()
app, document, candidates = sys.argv[2:5]
start_index = int(sys.argv[5]) if len(sys.argv) > 5 else 0
prefix = Path(os.environ.get("TUXOFFICE_PREFIX", ""))
if not str(prefix).endswith("/home/prefix"):
    sys.exit("Use the isolated prefix")
out.mkdir(parents=True, exist_ok=True)

work = prefix / "drive_c/tuxoffice/hunt" / app
work.mkdir(parents=True, exist_ok=True)
winwork = rf"C:\tuxoffice\hunt\{app}"
PROGID = {"word": "Word.Application", "excel": "Excel.Application", "powerpoint": "PowerPoint.Application",
          "publisher": "Publisher.Application", "access": "Access.Application",
          "outlook": "Outlook.Application"}[app]
EXE = {"word": "WINWORD.EXE", "excel": "EXCEL.EXE", "powerpoint": "POWERPNT.EXE", "publisher": "MSPUB.EXE",
       "access": "MSACCESS.EXE", "outlook": "OUTLOOK.EXE"}[app]
REGNAME = {"word": "Word", "excel": "Excel", "powerpoint": "PowerPoint", "publisher": "Publisher",
           "access": "Access", "outlook": "Outlook"}[app]
# Each app's way of opening the document, closing it, and saying whether one is open.
OPEN = {"word": "Set d=a.Documents.Open(doc)", "excel": "Set d=a.Workbooks.Open(doc)",
        "powerpoint": "Set d=a.Presentations.Open(doc)", "publisher": "Set d=a.Open(doc)",
        "access": "a.OpenCurrentDatabase doc",
        "outlook": 'Set d=a.GetNamespace("MAPI").GetDefaultFolder(6)\n  If a.Explorers.Count=0 Then d.Display\n'
                   '  Set cb=a.ActiveExplorer.CommandBars'}[app]
# Outlook keeps its one window: closing the last explorer would end it.
SHUT = {"access": "a.CloseCurrentDatabase", "outlook": "Err.Clear"}.get(app, "d.Saved=True\n      d.Close")
COUNT = {"word": "a.Documents.Count", "excel": "a.Workbooks.Count", "powerpoint": "a.Presentations.Count",
         "publisher": "a.Documents.Count", "access": "Len(a.CurrentProject.FullName)",
         "outlook": "a.Explorers.Count"}[app]
VISIBLE = {"publisher": "a.ActiveWindow.Visible=True", "outlook": ""}.get(app, "a.Visible=True")
COMMANDBARS = {"access": "", "outlook": ""}.get(app, "Set cb=a.CommandBars")
ALERTS = {"word": "a.DisplayAlerts=0", "excel": "a.DisplayAlerts=0"}.get(app, "")
MODE = "runcommand" if app == "access" else ("controls" if candidates == "controls" else "mso")

# Commands that end the session or close the document under test tell us nothing.
SKIP = re.compile(r"^(File(Exit|Close|CloseOrExit|Quit)|Quit|Exit.*|ApplicationExit|WindowClose.*|CloseWindow.*|"
                  r"Close(All|Document|Presentation|Workbook|Application).*|.*SignOut.*|.*Sign[Ii]n.*|Restart.*|"
                  r"MasterViewClose|ViewSlideShow.*|SlideShow.*)$")
# The same for the captions of numbered controls.
SKIP_CAPTION = re.compile(r"e&?x&?it|c&?l&?o&?s&?e|q&?uit|sign (in|out)|log ?(off|out)|restart|slide show|"
                          r"from (beginning|current)|&?run\b", re.I)

COMMON = r'''
Set fso=CreateObject("Scripting.FileSystemObject")
Sub Note(line)
  Dim f
  Set f=fso.OpenTextFile(progress,8,True)
  f.WriteLine line
  f.Close
End Sub
'''

VALIDATE = rf'''
progress="{winwork}\valid.txt"
{COMMON}
Set a=CreateObject("{PROGID}")
On Error Resume Next
{VISIBLE}
{ALERTS}
doc="{document}"
{OPEN}
{COMMANDBARS}
Set src=fso.OpenTextFile("{winwork}\candidates.txt",1)
Do While Not src.AtEndOfStream
  n=src.ReadLine
  Err.Clear
  v=cb.GetEnabledMso(n)
  If Err.Number=0 Then Note n
Loop
Err.Clear
Note "#done"
a.Quit
'''

VALIDATE_CONTROLS = rf'''
progress="{winwork}\valid.txt"
{COMMON}
Set a=CreateObject("{PROGID}")
On Error Resume Next
{VISIBLE}
{ALERTS}
doc="{document}"
{OPEN}
{COMMANDBARS}
For i=1 To 45000
  Err.Clear
  Set c=Nothing
  Set c=cb.FindControl(,i)
  If Err.Number=0 Then
    If Not c Is Nothing Then Note i & "|" & c.Caption
  End If
Next
Err.Clear
Note "#done"
a.Quit
'''

RUN_ONE = {
    "mso": '''enabled=cb.GetEnabledMso(n)
    If Err.Number<>0 Then enabled=False
    Err.Clear
    If enabled Then
      Note "run " & i & " " & n
      cb.ExecuteMso n
      Err.Clear
      WScript.Sleep 350
    End If''',
    "controls": '''Set c=Nothing
    Set c=cb.FindControl(,CLng(n))
    enabled=False
    If Err.Number=0 Then
      If Not c Is Nothing Then enabled=c.Enabled
    End If
    Err.Clear
    If enabled Then
      Note "run " & i & " " & n & " " & c.Caption
      c.Execute
      Err.Clear
      WScript.Sleep 300
    End If''',
    "runcommand": '''Note "run " & i & " " & n
    a.DoCmd.RunCommand CLng(n)
    Err.Clear
    WScript.Sleep 200''',
}[MODE]

RUN = rf'''
progress="{winwork}\progress.txt"
{COMMON}
Set a=CreateObject("{PROGID}")
On Error Resume Next
{VISIBLE}
{ALERTS}
doc="{document}"
{OPEN}
{COMMANDBARS}
first=CLng(WScript.Arguments(0))
Set src=fso.OpenTextFile("{winwork}\commands.txt",1)
i=0
Do While Not src.AtEndOfStream
  n=src.ReadLine
  If i>=first Then
    Err.Clear
    If {COUNT}=0 Then
      Err.Clear
      {OPEN}
    End If
    Err.Clear
    If (i-first) Mod 80=79 Then
      {SHUT}
      Err.Clear
      {OPEN}
      Err.Clear
    End If
    {RUN_ONE}
  End If
  i=i+1
Loop
Note "#done"
Err.Clear
d.Saved=True
a.Quit
'''


def wine(*args, **kwargs):
    return sp.run([tux, "wine", *args], capture_output=True, text=True, errors="replace", **kwargs)


def kill():
    sp.run([tux, "kill"], check=False, capture_output=True)
    wine("reg", "delete", rf"HKCU\Software\Microsoft\Office\16.0\{REGNAME}\Resiliency", "/f", timeout=30)
    sp.run([tux, "kill"], check=False, capture_output=True)
    time.sleep(1)


def pid_of_app():
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.joinpath("comm").read_text().strip() == EXE and \
                    str(prefix) in entry.joinpath("environ").read_bytes().decode(errors="replace"):
                return int(entry.name)
        except OSError:
            continue
    return None


def windows_pid(unix_pid):
    """The Windows id of the app, found from the helper's listing."""
    listing = wine("C:/tuxoffice/winctl.exe", "list", timeout=25).stdout
    for line in listing.splitlines():
        match = re.search(rf"{EXE}/(\d+)", line)
        if match:
            return match.group(1)
    return None


def launch(script, args, log):
    env = dict(os.environ, WINEDEBUG="-all,err+seh,warn+seh")
    return sp.Popen([tux, "wine", "cscript", "//nologo", script, *args], stdout=log, stderr=log, env=env)


def closer():
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if pid_of_app():
            pid = windows_pid(None)
            if pid:
                return sp.Popen([tux, "wine", "C:/tuxoffice/dialog-closer.exe", pid],
                                stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        time.sleep(2)
    return None


def stop(*processes):
    for process in processes:
        if process and process.poll() is None:
            process.kill()


def validate():
    commands = out / "commands.txt"
    if commands.exists():
        return [line for line in commands.read_text().splitlines() if line]
    if MODE == "runcommand":
        kept = [str(number) for number in range(1, 1000)
                if number not in (3, 4, 26, 57, 58, 584, 700)]  # exit, close and the like
        commands.write_text("\n".join(kept) + "\n")
        return kept
    if MODE == "mso":
        shutil.copy(candidates, work / "candidates.txt")
    (work / "validate.vbs").write_text(VALIDATE if MODE == "mso" else VALIDATE_CONTROLS)
    valid = work / "valid.txt"
    valid.unlink(missing_ok=True)
    kill()
    with (out / "validate.log").open("w") as log:
        process = launch(rf"{winwork}\validate.vbs", [], log)
        helper = closer()
        last, changed = 0, time.monotonic()
        while process.poll() is None and time.monotonic() - changed < 600:
            time.sleep(5)
            size = valid.stat().st_size if valid.exists() else 0
            if size != last:
                last, changed = size, time.monotonic()
        stop(process, helper)
    names = [line.strip() for line in valid.read_text(errors="replace").splitlines()
             if line.strip() and not line.startswith("#")]
    if MODE == "controls":
        (out / "controls.txt").write_text("\n".join(names) + "\n")
        kept = [name.split("|", 1)[0] for name in names if not SKIP_CAPTION.search(name.split("|", 1)[1])]
    else:
        kept = [name for name in names if not SKIP.match(name)]
    commands.write_text("\n".join(kept) + "\n")
    print(f"{app}: {len(names)} valid commands, {len(kept)} kept", flush=True)
    return kept


def finding(kind, command, index, seconds, log_text):
    faults = [line.strip() for line in log_text.splitlines()
              if "c0000005" in line or "Unhandled" in line or "c0000409" in line or "c0000374" in line]
    record = {"app": app, "kind": kind, "command": command, "index": index, "seconds": round(seconds, 1),
              "faults": faults[-6:]}
    with (out / "findings.jsonl").open("a") as file:
        file.write(json.dumps(record) + "\n")
    print(json.dumps(record)[:400], flush=True)


commands = validate()
(work / "commands.txt").write_text("\n".join(commands) + "\n")
(work / "run.vbs").write_text(RUN)
progress = work / "progress.txt"
index = start_index
HANG_SECONDS = 40
while index < len(commands):
    kill()
    progress.unlink(missing_ok=True)
    log_path = out / "run.log"
    start = time.monotonic()
    with log_path.open("w") as log:
        process = launch(rf"{winwork}\run.vbs", [str(index)], log)
        helper = closer()
        last_text, changed = "", time.monotonic()
        result = None
        while True:
            time.sleep(1)
            text = progress.read_text(errors="replace") if progress.exists() else ""
            if text != last_text:
                last_text, changed = text, time.monotonic()
            lines = text.splitlines()
            if lines and lines[-1] == "#done":
                result = "done"
                break
            if lines and not pid_of_app():
                result = "gone"
                break
            if process.poll() is not None:
                result = "client"
                break
            if time.monotonic() - changed > HANG_SECONDS:
                result = "hang"
                break
        stop(helper)
        if result == "hang":
            (out / f"hang-{index}.windows.txt").write_text(wine("C:/tuxoffice/winctl.exe", "list", timeout=25).stdout)
        stop(process)
    log_text = log_path.read_text(errors="replace")
    lines = [line for line in (progress.read_text(errors="replace") if progress.exists() else "").splitlines()
             if line.startswith("run ")]
    if result == "done":
        print(f"{app}: finished at {len(commands)} commands", flush=True)
        break
    if lines:
        _, reached, command = lines[-1].split(" ", 2)
        reached = int(reached)
    else:
        reached, command = index, "(startup)"
    if result != "client" or "c0000005" in log_text:
        finding(result, command, reached, time.monotonic() - start, log_text)
        shutil.copy(log_path, out / f"log-{reached}.txt")
    index = max(index + 1, reached + 1)
    print(f"{app}: resuming at {index}/{len(commands)}", flush=True)
kill()
