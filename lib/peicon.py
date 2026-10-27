# SPDX-License-Identifier: LGPL-2.1-or-later
"""Extract the largest icon from a PE executable's first icon group as PNG.

Usage: peicon.py APP.EXE OUT.png

Pure standard library so it works on any distro without pefile/icoutils.
Office embeds PNG-compressed 256px icons; BMP-only icons are wrapped in a
single-image .ico which most desktops also accept.
"""
import struct
import sys

RT_ICON, RT_GROUP_ICON = 3, 14


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    ddir = opt + (96 if magic == 0x10B else 112)
    rsrc_rva, _ = struct.unpack_from("<II", data, ddir + 2 * 8)
    secs = []
    for i in range(nsec):
        off = opt + optsize + i * 40
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        secs.append((va, max(vsize, rawsize), rawptr))
    return rsrc_rva, secs


def rva2off(rva, secs):
    for va, size, raw in secs:
        if va <= rva < va + size:
            return rva - va + raw
    raise ValueError("rva %x outside sections" % rva)


def entries(data, base, off):
    named, ids = struct.unpack_from("<HH", data, off + 12)
    for i in range(named + ids):
        name, target = struct.unpack_from("<II", data, off + 16 + i * 8)
        yield name, target


def resources(data, rtype):
    """Yield (id, bytes) for the first language of each resource of rtype."""
    rsrc_rva, secs = sections(data)
    base = rva2off(rsrc_rva, secs)
    for tname, ttarget in entries(data, base, base):
        if tname != rtype or not ttarget & 0x80000000:
            continue
        for rid, rtarget in entries(data, base, base + (ttarget & 0x7FFFFFFF)):
            lang = next(entries(data, base, base + (rtarget & 0x7FFFFFFF)))[1]
            drva, dsize = struct.unpack_from("<II", data, base + lang)
            doff = rva2off(drva, secs)
            yield rid, data[doff:doff + dsize]


def main(exe, out):
    data = open(exe, "rb").read()
    icons = dict(resources(data, RT_ICON))
    group = next(resources(data, RT_GROUP_ICON))[1]
    count, = struct.unpack_from("<H", group, 4)
    best = None
    for i in range(count):
        w, h, colors, _, planes, bpp, size, iid = struct.unpack_from("<BBBBHHIH", group, 6 + i * 14)
        score = ((w or 256) * (h or 256), bpp)
        if iid in icons and (best is None or score > best[0]):
            best = (score, group[6 + i * 14:6 + i * 14 + 12], icons[iid])
    if best is None:
        raise SystemExit("no icon found")
    _, entry, img = best
    with open(out, "wb") as f:
        if img[:8] == b"\x89PNG\r\n\x1a\n":
            f.write(img)
        else:
            f.write(struct.pack("<HHH", 0, 1, 1) + entry[:8] + struct.pack("<II", len(img), 22) + img)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
