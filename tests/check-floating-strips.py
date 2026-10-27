#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reject thin Office drawing helpers exposed as normal desktop windows."""
import json
from pathlib import Path
import subprocess
import tempfile

from office_windows import prefix_pids, classes

with tempfile.NamedTemporaryFile(mode="w",suffix=".js") as script:
    script.write('const pids='+json.dumps(prefix_pids())+'; const apps='+json.dumps(list(classes.values()))+'; for(const w of workspace.windowList()){if(pids.includes(w.pid) && apps.includes(w.resourceClass) && w.normalWindow && !w.minimized && !w.skipTaskbar && (w.caption === "" || w.caption === "DropShadowTop") && (w.frameGeometry.width <= 16 || w.frameGeometry.height <= 16))print("TUXSTRIP",JSON.stringify({caption:w.caption,app:w.resourceClass,pid:w.pid,width:w.frameGeometry.width,height:w.frameGeometry.height}));}')
    script.flush()
    output=subprocess.check_output([str(Path(__file__).with_name("kwin-run.sh")),script.name],text=True)
assert "TUXSTRIP " not in output, "Drawing helpers became desktop windows:\n"+output
print("No floating Office shadow strips")
