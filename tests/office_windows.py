# SPDX-License-Identifier: LGPL-2.1-or-later
"""Select KWin windows belonging to the chosen Office prefix."""
import json
import os
from pathlib import Path

classes={"word":"winword.exe","excel":"excel.exe","powerpoint":"powerpnt.exe"}


def prefix_pids(app=None):
    data=Path(os.environ.get("XDG_DATA_HOME",Path.home()/".local/share"))
    home=Path(os.environ.get("TUXOFFICE_HOME",data/"tuxoffice"))
    prefix=Path(os.environ.get("TUXOFFICE_PREFIX",str(home/"prefixes/office"))).resolve()
    pids=[]
    for process in Path("/proc").iterdir():
        if not process.name.isdigit(): continue
        try:
            command=(process/"cmdline").read_bytes().split(b"\0")[0].replace(b"\\",b"/")
            if app and command.rsplit(b"/",1)[-1].lower()!=classes[app].encode(): continue
            if not command.lower().endswith(b".exe"): continue
            env=(process/"environ").read_bytes().split(b"\0")
            entry=next((v for v in env if v.startswith(b"WINEPREFIX=")),None)
            if entry is None or Path(os.fsdecode(entry[11:])).resolve()!=prefix: continue
            pids.append(int(process.name))
        except OSError: pass
    return pids


def window_script(app,stem=None,activate=False,geometry=False):
    condition='w.normalWindow && !w.skipTaskbar && w.caption !== "DropShadowTop" && w.resourceClass === '+json.dumps(classes[app])
    if stem: condition+=' && w.caption.includes('+json.dumps(stem)+')'
    return ('const pids='+json.dumps(prefix_pids(app))+'; const candidates=workspace.windowList().filter(w => pids.includes(w.pid) && '+condition+'); '
            'const w=candidates.find(w => w===workspace.activeWindow) || candidates[0]; if(w){'
            +('w.minimized=false;workspace.raiseWindow(w);workspace.activeWindow=w;' if activate else '')
            +'print("TUXCAPTURE",w.internalId);'
            +('print("TUXGEOMETRY",JSON.stringify({x:w.frameGeometry.x,y:w.frameGeometry.y,width:w.frameGeometry.width,height:w.frameGeometry.height}));' if geometry else '')+'}')
