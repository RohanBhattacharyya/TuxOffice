#!/usr/bin/env python3
"""Register Click-to-Run's Windows Installer components that Wine cannot see.

Usage: c2rcomponents.py PREFIX OUTPUT.reg [HKLM-SOFTWARE.reg]

The registry is read from system.reg, which the wineserver writes only when
it exits, or from an export of HKLM\\Software ("wine reg export"), which
reflects a running prefix.

On Windows, Click-to-Run's App-V layer makes every component of its
integration manifests visible to Windows Installer queries. Under Wine only
the components the integration published to the real registry exist, so an
MSI query for any other one fails; Access then reports that its automatic
configuration failed. This writes registrations, in the form the published
ones use, for each manifest component whose key path exists in the prefix.
Existing registrations are left alone.
"""
import collections
import glob
import re
import sys
from pathlib import Path, PureWindowsPath

prefix = Path(sys.argv[1])
output = Path(sys.argv[2])
drive_c = prefix / "drive_c"
root = r"C:\Program Files\Microsoft Office\root"
COMPONENTS = r"Software\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Components"
TOKENS = {
    "%SFT_PROGRAM_FILES_X64%": [root + r"\VFS\ProgramFilesX64", r"C:\Program Files"],
    "%SFT_PROGRAM_FILES_COMMON_X64%": [root + r"\VFS\ProgramFilesCommonX64", r"C:\Program Files\Common Files"],
    "%CSIDL_PROGRAM_FILES%": [root + r"\VFS\ProgramFilesX86", r"C:\Program Files (x86)"],
    "%CSIDL_PROGRAM_FILES_COMMON%": [root + r"\VFS\ProgramFilesCommonX86", r"C:\Program Files (x86)\Common Files"],
    "%SFT_SYSTEM32_X64%": [root + r"\VFS\System", r"C:\windows\system32"],
    "%CSIDL_SYSTEM%": [root + r"\VFS\SystemX86", r"C:\windows\syswow64"],
    "%CSIDL_FONTS%": [root + r"\VFS\Fonts", r"C:\windows\Fonts"],
    "%CSIDL_WINDOWS%": [root + r"\VFS\Windows", r"C:\windows"],
}
# Office's own folder is mapped onto the root, not into the VFS tree.
SPECIAL = [("%SFT_PROGRAM_FILES_X64%\\Microsoft Office\\", root + "\\")]
REG_ROOTS = {"0": r"Software\Classes", "2": "Software"}


def squash(guid):
    g = guid.strip("{}").replace("-", "")
    head = g[:8][::-1] + g[8:12][::-1] + g[12:16][::-1]
    return (head + "".join(g[i + 1] + g[i] for i in range(16, 32, 2))).upper()


def read_text(path):
    data = path.read_bytes()
    return data.decode("utf-16") if data[:2] in (b"\xff\xfe", b"\xfe\xff") else data.decode("utf-8", "replace")


def to_unix(windows_path):
    parts = PureWindowsPath(windows_path).parts[1:]
    current = drive_c
    for part in parts:
        if not current.is_dir():
            return None
        exact = current / part
        if exact.exists():
            current = exact
            continue
        match = next((c for c in current.iterdir() if c.name.lower() == part.lower()), None)
        if match is None:
            return None
        current = match
    return current


if len(sys.argv) > 3:
    # "[HKEY_LOCAL_MACHINE\Software\Key]" -> system.reg's "[Software\\Key]"
    registry = re.sub(r"^\[HKEY_LOCAL_MACHINE\\([^\]\n]*)\]",
                      lambda m: "[" + m[1].replace("\\", "\\\\") + "]",
                      read_text(Path(sys.argv[3])).replace("\r\n", "\n"), flags=re.M | re.I)
else:
    registry = (prefix / "system.reg").read_text(encoding="utf-8", errors="replace")
keys = {m.lower() for m in re.findall(r"^\[([^\]]+)\]", registry, re.M)}
published = {}
# system.reg doubles each backslash in key names.
stored = re.escape(COMPONENTS.replace("\\", "\\\\") + "\\\\")
for m in re.finditer(r"^\[" + stored + r"([0-9A-F]{32})\][^\n]*\n((?:[#\"@][^\n]*\n)*)", registry, re.M | re.I):
    published[m[1].upper()] = re.findall(r'^"([0-9A-Fa-f]{32})"=', m[2], re.M)


def resolve(key_path):
    """The registered value for a key path, or None if it does not exist."""
    m = re.match(r"([0-3])([0-3]):\\(.*)", key_path)
    if m:
        base = REG_ROOTS.get(m[2])
        if base is None:
            return None
        key = (base + "\\" + m[3].rstrip("\\")).replace("\\", "\\\\").lower()
        return key_path if key in keys else None
    for token, target in SPECIAL:
        if key_path.lower().startswith(token.lower()):
            candidate = target + key_path[len(token):]
            if to_unix(candidate.rstrip("\\")):
                return candidate
    m = re.match(r"(%[A-Z0-9_]+%)(.*)", key_path)
    if not m or m[1] not in TOKENS:
        return None
    for base in TOKENS[m[1]]:
        candidate = base + m[2]
        if to_unix(candidate.rstrip("\\")):
            return candidate
    return None


components = {}
for manifest in glob.glob(str(drive_c / "Program Files/Microsoft Office/root/Integration/C2RManifest*.xml")):
    for m in re.finditer(r'<Component ComponentId="(\{[^}]+\})" KeyPath="([^"]*)"', read_text(Path(manifest))):
        components.setdefault(m[1].upper(), m[2].replace("&amp;", "&"))
# The Click-to-Run product that owns the published manifest components.
owners = collections.Counter(name for guid in components for name in published.get(squash(guid), []))
assert owners, "No published Click-to-Run components; is Office installed?"
product = owners.most_common(1)[0][0]
lines = ["Windows Registry Editor Version 5.00", ""]
added = skipped = 0
for guid, key_path in sorted(components.items()):
    if squash(guid) in published:
        continue
    value = resolve(key_path)
    if value is None:
        skipped += 1
        continue
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    lines += [f"[HKEY_LOCAL_MACHINE\\{COMPONENTS}\\{squash(guid)}]",
              f'"{product}"="{escaped}"', ""]
    added += 1
output.write_text("\r\n".join(lines) + "\r\n", encoding="utf-16")
print(f"{added} components to register, {skipped} without an existing key path, "
      f"{len(published)} already published, product {product}")
