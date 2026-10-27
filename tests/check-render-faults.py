#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Detect graphics access violations even when Office catches the exception."""
from pathlib import Path
import re
import sys

text = Path(sys.argv[1]).read_text(errors="replace")
for block in text.split("backtrace: --- Exception ")[1:]:
    if block.startswith("0xc0000005") and re.search(r'L"(?:windows\.ui|d3d11|d2d1)\.dll"', block, re.I):
        print("Graphics access violation recorded in " + sys.argv[1])
        sys.exit(1)
