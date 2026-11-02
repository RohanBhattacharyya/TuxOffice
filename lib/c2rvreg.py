#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read Click-to-Run's virtual registry from the downloaded Office payload.

Usage: c2rvreg.py PREFIX OUTPUT.reg

Office keeps most of its registry in per-package hives (root\\vreg\\*.vreg.dat),
which Click-to-Run merges into VirtualRegistry.dat and App-V overlays on the
registry of Office's processes. Under Wine the merge leaves empty hives, so
everything in them is missing: Access's wizards, VBA's location, Outlook's
services and much more. The originals are still in the payload. This reads them
through Click-to-Run's own database (each block is zlib data whose SHA-256 is
recorded), parses the registry hive format and writes a .reg file for the place
Click-to-Run mounts the merged hive on Windows.
"""
import base64
import hashlib
import os
import re
import sqlite3
import struct
import sys
import zlib

QUERY = """
select p.Name, m.Name, whole.Size, whole_digest.Digest, source.Offset, source.Size, block_digest.Digest
from Files f
join Paths p on p.path_id = f.target_path
join Directories d on d.directory_id = p.directory_id
join Media m on m.media_id = f.media_id
join Hashes whole on whole.hash_id = f.hash_id
join Digests whole_digest on whole_digest.digest_id = whole.digest_id
join Blocks b on b.file_id = f.file_id
join Hashes block on block.hash_id = b.hash_id
join Digests block_digest on block_digest.digest_id = block.digest_id
join BlockSource s on s.block_id = b.block_id
join Hashes source on source.hash_id = s.hash_id
where d.Name = 'root\\vreg' and p.Name like '%.vreg.dat'
order by p.Name, block.Offset
"""
ROOTS = {"MACHINE": "HKEY_LOCAL_MACHINE", "USER": "HKEY_USERS"}


def read_hives(database, stream_dir):
    """Yield (name, bytes) for every hive whose stream is present."""
    connection = sqlite3.connect(f"file:{database}?mode=ro&immutable=1", uri=True)
    files = {}
    for name, media, size, digest, offset, length, block_digest in connection.execute(QUERY):
        files.setdefault(name, (media, size, digest, []))[3].append((offset, length))
    streams = {}
    try:
        for name, (media, size, digest, blocks) in sorted(files.items()):
            path = os.path.join(stream_dir, media)
            if not os.path.isfile(path):
                continue
            stream = streams.setdefault(media, open(path, "rb"))
            data = bytearray()
            for offset, length in blocks:
                stream.seek(offset)
                data += zlib.decompress(stream.read(length))
            data = bytes(data)
            expected = base64.b64decode(digest)
            if len(data) != size or hashlib.sha256(data).digest() != expected:
                raise ValueError(f"{name}: payload does not match Click-to-Run's digest")
            yield name, data
    finally:
        for stream in streams.values():
            stream.close()
        connection.close()


class Hive:
    BASE = 0x1000

    def __init__(self, data):
        if data[:4] != b"regf":
            raise ValueError("not a registry hive")
        self.data = data
        self.root = struct.unpack_from("<I", data, 0x24)[0]

    def cell(self, offset):
        return self.BASE + offset + 4

    def subkeys(self, offset):
        data, position = self.data, self.cell(offset)
        signature = data[position:position + 2]
        count = struct.unpack_from("<H", data, position + 2)[0]
        if signature in (b"lf", b"lh"):
            for i in range(count):
                yield struct.unpack_from("<I", data, position + 4 + i * 8)[0]
        elif signature == b"li":
            for i in range(count):
                yield struct.unpack_from("<I", data, position + 4 + i * 4)[0]
        elif signature == b"ri":
            for i in range(count):
                yield from self.subkeys(struct.unpack_from("<I", data, position + 4 + i * 4)[0])
        else:
            raise ValueError(f"unknown subkey list {signature!r}")

    def value_data(self, size, offset, value_position):
        data = self.data
        if size & 0x80000000:
            return data[value_position + 8:value_position + 8 + (size & 0x7fffffff)]
        position = self.cell(offset)
        if data[position:position + 2] == b"db":
            # Big data: a list of segments of up to 16344 bytes.
            count, segments = struct.unpack_from("<HI", data, position + 2)
            result = bytearray()
            list_position = self.cell(segments)
            for i in range(count):
                segment = self.cell(struct.unpack_from("<I", data, list_position + i * 4)[0])
                result += data[segment:segment + min(16344, size - len(result))]
            return bytes(result)
        return data[position:position + size]

    def walk(self, offset=None, path=()):
        """Yield (key path, [(name, type, data)]) depth first."""
        data = self.data
        position = self.cell(self.root if offset is None else offset)
        if data[position:position + 2] != b"nk":
            raise ValueError("corrupt key")
        flags = struct.unpack_from("<H", data, position + 2)[0]
        subkey_count = struct.unpack_from("<I", data, position + 0x14)[0]
        subkey_list = struct.unpack_from("<I", data, position + 0x1c)[0]
        value_count = struct.unpack_from("<I", data, position + 0x24)[0]
        value_list = struct.unpack_from("<I", data, position + 0x28)[0]
        length = struct.unpack_from("<H", data, position + 0x48)[0]
        raw = data[position + 0x4c:position + 0x4c + length]
        name = raw.decode("latin-1") if flags & 0x20 else raw.decode("utf-16-le")
        here = path + (name,) if offset is not None else ()
        values = []
        if value_count:
            list_position = self.cell(value_list)
            for i in range(value_count):
                value = self.cell(struct.unpack_from("<I", data, list_position + i * 4)[0])
                name_length = struct.unpack_from("<H", data, value + 2)[0]
                size, where, kind = struct.unpack_from("<III", data, value + 4)
                value_flags = struct.unpack_from("<H", data, value + 16)[0]
                raw_name = data[value + 20:value + 20 + name_length]
                value_name = raw_name.decode("latin-1" if value_flags & 1 else "utf-16-le")
                values.append((value_name, kind, self.value_data(size, where, value)))
        yield here, values
        if subkey_count:
            for child in self.subkeys(subkey_list):
                yield from self.walk(child, here)


TOKENS = {
    "[{AppVPackageRoot}]": r"C:\Program Files\Microsoft Office\root",
    "[{ProgramFilesCommonX64}]": r"C:\Program Files\Common Files",
    "[{ProgramFilesCommonX86}]": r"C:\Program Files (x86)\Common Files",
    "[{ProgramFilesX64}]": r"C:\Program Files",
    "[{ProgramFilesX86}]": r"C:\Program Files (x86)",
    "[{Windows}]": r"C:\windows",
    "[{System}]": r"C:\windows\system32",
    "[{SystemX86}]": r"C:\windows\syswow64",
    "[{Common AppData}]": r"C:\ProgramData",
    "[{Fonts}]": r"C:\windows\Fonts",
}
TEXT_TYPES = (1, 2, 7)


# Under App-V a tokenized folder is the real folder merged with the package's VFS
# folder of that name. Wine has no such redirection, so a path that only the
# package supplies has to name its place in the VFS folder directly.
VFS_ROOT = TOKENS["[{AppVPackageRoot}]"] + r"\VFS"
TOKENIZED_PATH = re.compile(r'\[\{([^}\]]+)\}\]([^",;|]*)')
vfs_directory = None


def in_vfs(folder, path):
    """Whether the VFS folder holds this path, compared without regard to case."""
    current = os.path.join(vfs_directory, folder)
    for part in filter(None, path.split("\\")):
        try:
            names = {name.lower(): name for name in os.listdir(current)}
        except OSError:
            return False
        if part.lower() not in names:
            return False
        current = os.path.join(current, names[part.lower()])
    return True


def expand_token(match):
    folder, path = match.groups()
    token = f"[{{{folder}}}]"
    if token not in TOKENS:
        return match.group(0)
    # The last part may be a file followed by arguments.
    parent = path.rpartition("\\")[0]
    if vfs_directory and folder != "AppVPackageRoot" and parent.strip("\\") and \
            (in_vfs(folder, path) or in_vfs(folder, parent)):
        return f"{VFS_ROOT}\\{folder}{path}"
    return TOKENS[token] + path


def detokenize(text):
    return TOKENIZED_PATH.sub(expand_token, text) if "[{" in text else text


def reg_string(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def reg_value(name, kind, data):
    label = '@' if name == "" else f'"{reg_string(detokenize(name))}"'
    if kind == 1:
        text = data.decode("utf-16-le", "surrogatepass").split("\0")[0]
        if "\n" not in text and "\r" not in text:
            return f'{label}="{reg_string(detokenize(text))}"'
        data = (detokenize(text) + "\0").encode("utf-16-le", "surrogatepass")
    elif kind == 4 and len(data) == 4:
        return f"{label}=dword:{struct.unpack('<I', data)[0]:08x}"
    elif kind in TEXT_TYPES:
        data = detokenize(data.decode("utf-16-le", "surrogatepass")).encode("utf-16-le", "surrogatepass")
    code = "hex" if kind == 3 else f"hex({kind:x})"
    return f"{label}={code}:" + ",".join(f"{byte:02x}" for byte in data)


def merge(hives):
    """Combine hives in Click-to-Run's order: later files win."""
    merged = {}
    for data in hives:
        for path, entries in Hive(data).walk():
            if not path or path[0].upper() != "REGISTRY":
                continue
            key = merged.setdefault("\\".join(path[1:]).lower(), ("\\".join(path[1:]), {}))
            for name, kind, value in entries:
                key[1][name.lower()] = (name, kind, value)
    return merged


def write_reg(merged, output):
    """The .reg file adds Click-to-Run's mounted hive, where App-V reads it."""
    keys = values = 0
    root = r"HKEY_LOCAL_MACHINE\Software\Microsoft\Office\ClickToRun\REGISTRY"
    with open(output, "w", encoding="utf-16", newline="\r\n") as out:
        out.write("Windows Registry Editor Version 5.00\n\n")
        for _, (path, entries) in sorted(merged.items()):
            if not path:
                continue
            out.write(f"[{root}\\{detokenize(path)}]\n")
            keys += 1
            for name, kind, value in entries.values():
                out.write(reg_value(name, kind, value) + "\n")
                values += 1
            out.write("\n")
    return keys, values


def windows_to_unix(prefix, path):
    """C:\\tuxoffice\\source\\... inside the prefix."""
    if path[1:3] != ":\\" or path[0].lower() != "c":
        return None
    return os.path.join(prefix, "drive_c", *path[3:].split("\\"))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    prefix, output = sys.argv[1:]
    releases = os.path.join(prefix, "drive_c/ProgramData/Microsoft/ClickToRun/ProductReleases")
    databases = sorted((os.path.join(releases, name, "operations.db") for name in os.listdir(releases)),
                       key=lambda path: os.path.getmtime(path) if os.path.exists(path) else 0)
    if not databases or not os.path.exists(databases[-1]):
        sys.exit("no Click-to-Run operations database in the prefix")
    database = databases[-1]
    connection = sqlite3.connect(f"file:{database}?mode=ro&immutable=1", uri=True)
    uris = [row[0] for row in connection.execute("select URI from Media")]
    connection.close()
    stream_dir = None
    for uri in uris:
        path = windows_to_unix(prefix, uri)
        if path and os.path.isfile(path):
            stream_dir = os.path.dirname(path)
            break
    if not stream_dir:
        sys.exit(f"Office's payload is not available (looked for {uris[0] if uris else 'none'})")
    global vfs_directory
    office_root = os.path.join(prefix, "drive_c/Program Files/Microsoft Office/root")
    vfs_directory = os.path.join(office_root, "vfs")
    installed = os.path.join(office_root, "vreg")
    # Only the packages this installation selected: their names are in root\vreg.
    wanted = {name.lower() for name in os.listdir(installed) if name.lower().endswith(".vreg.dat")}
    found = {name: data for name, data in read_hives(database, stream_dir) if name.lower() in wanted}
    missing = wanted - {name.lower() for name in found}
    if missing:
        sys.exit(f"payload lacks {len(missing)} hives, for example {sorted(missing)[0]}")
    merged = merge(found[name] for name in sorted(found, key=str.lower))
    keys, values = write_reg(merged, output)
    print(f"{len(found)} hives: {keys} keys, {values} values")


if __name__ == "__main__":
    main()
