#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check WIDL's optional type-index ABI file and its input validation.

Usage: python3 tests/typelib-order.py /path/to/built/tools/widl/widl
The normal compiler order must remain unchanged when the option is absent.
"""
from pathlib import Path
import json
import struct
import subprocess as sp
import sys
import tempfile

widl = str(Path(sys.argv[1]).resolve())
checks = 0
source = """
typedef [uuid(11111111-1111-1111-1111-111111111111)] enum First { Value1=1 } First;
typedef [uuid(22222222-2222-2222-2222-222222222222)] enum Second { Value2=2 } Second;
typedef [uuid(33333333-3333-3333-3333-333333333333)] struct Third
{ Second first; First one; } Third;
[uuid(44444444-4444-4444-4444-444444444444), version(1.0)] library Test
{ struct Third; enum First; enum Second; };
"""


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)


def names(path):
    data = path.read_bytes()
    check(data[:4] == b"MSFT", "Not an MSFT type library")
    count = struct.unpack_from("<I", data, 0x20)[0]
    table = 84 + (4 if struct.unpack_from("<I", data, 0x14)[0] & 0x100 else 0)
    directory = table + count * 4
    type_segment = struct.unpack_from("<I", data, directory)[0]
    name_segment = struct.unpack_from("<I", data, directory + 7 * 16)[0]
    result = []
    for i in range(count):
        offset = type_segment + struct.unpack_from("<I", data, table + i * 4)[0]
        check(struct.unpack_from("<I", data, offset)[0] >> 16 == i,
              "Description index does not match the index table")
        name = name_segment + struct.unpack_from("<I", data, offset + 0x34)[0]
        length = data[name + 8]
        result.append(data[name + 12:name + 12 + length].decode())
    return result


with tempfile.TemporaryDirectory(prefix="tuxoffice-widl-order-") as directory:
    work = Path(directory)
    idl, order, out = work / "test.idl", work / "test.order", work / "test.tlb"
    idl.write_text(source)
    for bits in (32, 64):
        command = [widl, f"-m{bits}", "-t", "-o", str(out)]
        sp.run([*command, str(idl)], check=True, capture_output=True, text=True)
        check(names(out) == ["Third", "Second", "First"], "Default order or declared name changed")
        order.write_text("# Explicit ABI order\nSecond\nThird # includes two referenced enums\nFirst\n")
        sp.run([*command, f"--typelib-order={order}", str(idl)], check=True,
               capture_output=True, text=True)
        check(names(out) == ["Second", "Third", "First"], "Explicit order not preserved")
        for contents, diagnostic in (
            ("First\nFirst\nThird\n", "duplicate type"),
            ("First\nMissing\nThird\n", "unknown type"),
            ("First\nSecond\n", "expected 3"),
            ("First unexpected\nSecond\nThird\n", "unexpected text"),
        ):
            order.write_text(contents)
            result = sp.run([*command, f"--typelib-order={order}", str(idl)],
                            capture_output=True, text=True)
            check(result.returncode != 0 and diagnostic in result.stderr, result.stderr)
        for options in (["--oldtlb", f"--typelib-order={order}"],
                        [f"--typelib-order={order}", "--oldtlb"]):
            result = sp.run([*command, *options, str(idl)], capture_output=True, text=True)
            check(result.returncode != 0 and "requires the MSFT format" in result.stderr,
                  result.stderr)
print(json.dumps({"status": "passed", "architectures": 2, "checks": checks}))
