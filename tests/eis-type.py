#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Turn text and key names into tests/eis-input.c commands (US layout).

Usage: eis-type.py ITEM... | eis-input
Each ITEM is literal text, or a key in braces: {enter} {escape} {tab}
{ctrl+a} {backspace}. Used to drive native (non-Wine) dialogs, such as the
desktop file chooser, inside the isolated session.
"""
import re
import sys

KEYS = {**{c: 30 + i for i, c in enumerate("asdfghjkl")}, **{c: 16 + i for i, c in enumerate("qwertyuiop")},
        **{c: 44 + i for i, c in enumerate("zxcvbnm")}, **{str(d): 2 + (d - 1) % 10 for d in range(1, 11)},
        "0": 11, "-": 12, "=": 13, "[": 26, "]": 27, ";": 39, "'": 40, "`": 41, "\\": 43,
        ",": 51, ".": 52, "/": 53, " ": 57}
SHIFTED = {"_": "-", "+": "=", "{": "[", "}": "]", ":": ";", '"': "'", "~": "`", "|": "\\",
           "<": ",", ">": ".", "?": "/", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
           "^": "6", "&": "7", "*": "8", "(": "9", ")": "0"}
NAMED = {"enter": 28, "escape": 1, "tab": 15, "backspace": 14}
SHIFT, CTRL = 42, 29


def press(code, modifiers=()):
    for m in modifiers:
        print(f"key {m} 1")
    print(f"key {code} 1")
    print(f"key {code} 0")
    for m in reversed(modifiers):
        print(f"key {m} 0")
    print("sleep 15")


for item in sys.argv[1:]:
    for token in re.findall(r"\{[^}]+\}|[^{]+", item):
        if token.startswith("{"):
            name = token[1:-1]
            if name.startswith("ctrl+"):
                press(KEYS[name[5:]], (CTRL,))
            else:
                press(NAMED[name])
            continue
        for c in token:
            if c in KEYS:
                press(KEYS[c])
            elif c.lower() in KEYS:
                press(KEYS[c.lower()], (SHIFT,))
            elif c in SHIFTED:
                press(KEYS[SHIFTED[c]], (SHIFT,))
            else:
                sys.exit(f"cannot type {c!r}")
