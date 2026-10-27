# SPDX-License-Identifier: LGPL-2.1-or-later
"""Cold-start prefetch from Soda launch traces.

Usage: readahead.py TRACE_DIR LIST

Soda's loader writes, per process, the Unix paths of every module it maps
during the first 45 s (SODA_ADAPTIVE_TRACE_DIR, "SODAAL2" header, NUL
separated). We merge those traces into LIST, then ask the kernel to read the
files into the page cache (POSIX_FADV_WILLNEED) while Wine is still starting,
so Office's ~200 startup DLLs come from RAM instead of disk on a cold cache.
"""
import os
import sys

MAGIC = b"SODAAL2\0"


def merge(trace_dir, list_path):
    paths = []
    if os.path.exists(list_path):
        paths = open(list_path, "rb").read().split(b"\0")
    seen = set(paths)
    for name in sorted(os.listdir(trace_dir)):
        if not name.endswith(".trace"):
            continue
        full = os.path.join(trace_dir, name)
        data = open(full, "rb").read()
        if data.startswith(MAGIC):
            for p in data[len(MAGIC):].split(b"\0"):
                if p and p not in seen:
                    seen.add(p)
                    paths.append(p)
        os.unlink(full)
    paths = [p for p in paths if p and os.path.isfile(p)]
    tmp = list_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"\0".join(paths))
    os.replace(tmp, list_path)
    return paths


def prefetch(paths):
    for p in paths:
        try:
            fd = os.open(p, os.O_RDONLY)
        except OSError:
            continue
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_WILLNEED)
        finally:
            os.close(fd)


if __name__ == "__main__":
    trace_dir, list_path = sys.argv[1:3]
    os.makedirs(trace_dir, exist_ok=True)
    prefetch(merge(trace_dir, list_path))
