#!/usr/bin/env python3
"""Run a command in an already running isolated-session.sh environment."""
import json
import os
from pathlib import Path
import sys

with (Path(sys.argv[1]) / 'environment.json').open() as f:
    os.environ.update(json.load(f))
os.environ.pop('DISPLAY', None)
# Screen checks compare colours: run Office in its light theme whatever the desktop prefers.
os.environ.setdefault('TUXOFFICE_THEME', 'light')
os.execvp(sys.argv[2], sys.argv[2:])
