#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Turn text and key names into tests/eis-input.c commands (US layout).

Usage: eis-type.py ITEM... | eis-input
Each ITEM is literal text, or a key in braces: {enter} {escape} {tab}
{ctrl+a} {ctrl+alt+m} {shift+f10} {f5} {backspace}. Used to drive native (non-Wine) dialogs, such as the
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
NAMED = {"enter": 28, "escape": 1, "tab": 15, "backspace": 14, "delete": 111, "home": 102, "end": 107,
         "up": 103, "down": 108, "left": 105, "right": 106, "pageup": 104, "pagedown": 109,
         **{f"f{i}": 58 + i for i in range(1, 11)}, "f11": 87, "f12": 88}
SHIFT, CTRL, ALT = 42, 29, 56
MODIFIERS = {"ctrl": CTRL, "shift": SHIFT, "alt": ALT}


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
            *mods, name = token[1:-1].split("+")
            press(NAMED[name] if name in NAMED else KEYS[name], tuple(MODIFIERS[m] for m in mods))
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
