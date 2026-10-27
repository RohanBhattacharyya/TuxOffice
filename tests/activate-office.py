#!/usr/bin/env python3
"""Activate an Office document in the selected prefix, leaving other prefixes alone."""
from pathlib import Path
import subprocess as sp
import sys
import tempfile

from office_windows import classes,window_script

app=sys.argv[1] if len(sys.argv)>1 else "word"
if app not in classes: sys.exit("Unknown Office app")
with tempfile.NamedTemporaryFile(mode="w",suffix=".js") as script:
    script.write(window_script(app,sys.argv[2] if len(sys.argv)>2 else None,True));script.flush()
    result=sp.run([str(Path(__file__).with_name("kwin-run.sh")),script.name],capture_output=True,text=True,check=True)
if "TUXCAPTURE " not in result.stdout: sys.exit("Selected Office document window was not found")
