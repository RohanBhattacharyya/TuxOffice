#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""List what Office's binaries import that the runner does not provide.

Usage: import-gaps.py OFFICE_ROOT RUNNER_DLL_DIR PREFIX_SYSTEM32 [FILTER...]

Under Wine, calling an import that does not exist ends the process: a missing function is
an "unimplemented function" stub, and a delay-loaded DLL that cannot be found raises
0xc06d007f. Both happen only when the code runs, so this finds them before a user does.
Imports by ordinal are shown as #N. Office's own DLLs are skipped, and so are api-ms-win-* sets. FILTER limits the report to
importing modules whose names contain one of the given strings, such as mso or excel.
"""
import collections
import os
import struct
import sys


def load(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 24)[0] != 0x20b:
        return None
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    sections, offset = [], pe + 24 + optional
    for _ in range(count):
        size, rva, raw_size, raw = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((rva, max(size, raw_size), raw))
        offset += 40

    def to_offset(rva):
        for start, size, raw in sections:
            if start <= rva < start + size:
                return rva - start + raw
        return None

    def string(rva):
        start = to_offset(rva)
        if start is None:
            return "?"
        return data[start:data.index(b"\0", start)].decode("latin-1")

    return data, pe + 24 + 112, to_offset, string


exports_cache = {}


def exports(path):
    if path in exports_cache:
        return exports_cache[path]
    names = set()
    image = load(path)
    if image:
        data, directories, to_offset, string = image
        rva = struct.unpack_from("<I", data, directories)[0]
        start = to_offset(rva) if rva else None
        if start is not None:
            base, functions, count = struct.unpack_from("<III", data, start + 16)
            addresses = to_offset(struct.unpack_from("<I", data, start + 28)[0])
            table = to_offset(struct.unpack_from("<I", data, start + 32)[0])
            for i in range(count):
                names.add(string(struct.unpack_from("<I", data, table + 4 * i)[0]))
            for i in range(functions):
                if struct.unpack_from("<I", data, addresses + 4 * i)[0]:
                    names.add(base + i)
    exports_cache[path] = names
    return names


def imports(path):
    image = load(path)
    if not image:
        return []
    data, directories, to_offset, string = image
    found = []
    for index, size, delayed in ((1, 20, False), (13, 32, True)):
        rva = struct.unpack_from("<I", data, directories + index * 8)[0]
        offset = to_offset(rva) if rva else None
        while offset is not None:
            if delayed:
                _, name, _, _, names, *_ = struct.unpack_from("<8I", data, offset)
            else:
                names, _, _, name, thunks = struct.unpack_from("<5I", data, offset)
                names = names or thunks
            if not name:
                break
            functions, entry = [], to_offset(names)
            while entry is not None:
                value = struct.unpack_from("<Q", data, entry)[0]
                if not value:
                    break
                functions.append(value & 0xffff if value >> 63 else string((value & 0x7fffffff) + 2))
                entry += 8
            found.append((string(name).lower(), delayed, functions))
            offset += size
    return found


office_root, runner, system32 = sys.argv[1:4]
filters = [f.lower() for f in sys.argv[4:]]
office = {}
for directory, _, files in os.walk(office_root):
    for name in files:
        if name.lower().endswith((".dll", ".exe")):
            office.setdefault(name.lower(), os.path.join(directory, name))
provided = {name.lower(): os.path.join(runner, name) for name in os.listdir(runner)}
provided.update({name.lower(): os.path.join(system32, name) for name in os.listdir(system32)
                 if name.lower() not in provided})

missing_dlls = collections.defaultdict(set)
missing_functions = collections.defaultdict(set)
for module, path in sorted(office.items()):
    if filters and not any(f in module for f in filters):
        continue
    for dll, delayed, functions in imports(path):
        if dll in office or dll.startswith(("api-ms-", "ext-ms-")):
            continue
        target = provided.get(dll)
        if not target:
            missing_dlls[(dll, delayed)].add(module)
            continue
        available = exports(target)
        if not available:
            continue
        for function in functions:
            if function not in available:
                missing_functions[(dll, str(function) if isinstance(function, str) else f"#{function}")].add(
                    module + (" (delay)" if delayed else ""))

print("Missing DLLs:")
for (dll, delayed), users in sorted(missing_dlls.items()):
    print(f"  {dll} {'delay' if delayed else 'static'}: {', '.join(sorted(users))}")
print("Missing functions:")
for (dll, function), users in sorted(missing_functions.items()):
    print(f"  {dll}!{function}: {', '.join(sorted(users))}")
