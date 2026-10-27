"""Print Windows font-registry entries for font files.

Usage: fontnames.py [--paths] FILE...
Output, one per line:  <registry value name>\t<file name>   (full path with --paths)
e.g. "Segoe UI Variable Text Semibold (TrueType)\tSegoe-UI-Variable-Static-Text-Semibold.ttf"

Reads the OpenType 'name' table (full font name, ID 4, Windows platform),
joining TrueType Collection members with " & " the way Windows does.
Standard library only.
"""
import struct
import sys


def font_offsets(data):
    if data[:4] == b"ttcf":
        count, = struct.unpack_from(">I", data, 8)
        return list(struct.unpack_from(">%dI" % count, data, 12))
    return [0]


def full_name(data, base):
    numtables, = struct.unpack_from(">H", data, base + 4)
    for i in range(numtables):
        tag, _, offset, _ = struct.unpack_from(">4sIII", data, base + 12 + i * 16)
        if tag != b"name":
            continue
        _, count, strings = struct.unpack_from(">HHH", data, offset)
        best = None
        for j in range(count):
            platform, encoding, lang, nameid, length, off = struct.unpack_from(">6H", data, offset + 6 + j * 12)
            if nameid != 4 or platform != 3 or encoding not in (0, 1, 10):
                continue
            raw = data[offset + strings + off:offset + strings + off + length]
            name = raw.decode("utf-16-be", "replace")
            if lang == 0x409:
                return name
            best = best or name
        return best
    return None


def main(paths, full_paths=False):
    for path in paths:
        try:
            data = open(path, "rb").read()
            names = [n for n in (full_name(data, o) for o in font_offsets(data)) if n]
        except (OSError, struct.error):
            continue
        if not names:
            continue
        kind = "OpenType" if path.lower().endswith(".otf") else "TrueType"
        print("%s (%s)\t%s" % (" & ".join(names), kind, path if full_paths else path.rsplit("/", 1)[-1]))


if __name__ == "__main__":
    args = sys.argv[1:]
    full = bool(args) and args[0] == "--paths"
    main(args[1:] if full else args, full)
