# SPDX-License-Identifier: LGPL-2.1-or-later
"""Download an Office Click-to-Run source tree natively.

Usage: c2rfetch.py DEST CHANNEL ARCH LANG [LANG...]
Prints the downloaded build version on stdout.

Click-to-Run's own downloader under Wine manages ~100 KiB/s; plain parallel
curl against the same CDN saturates the link. The resulting tree is the
layout ODT expects for <Add SourcePath=...>.
"""
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

RELEASES = "https://clients.config.office.net/releases/v1.0/OfficeReleases"
CHANNELS = {
    "Current": "492350f6-3a01-4f97-b9c0-c7c6ddf67d60",
    "MonthlyEnterprise": "55336b82-a18d-4dd6-b5f6-9e5095c314a6",
    "SemiAnnual": "7ffbc6bf-bc32-4f92-8982-f9dd17fd3114",
    "PerpetualVL2024": "7983bac0-e531-40cf-be00-fd24fe66619c",
    "PerpetualVL2021": "5030841d-c919-4594-8d2d-84ae4f96e58e",
}
LCIDS = {
    "en-us": 1033, "en-gb": 2057, "de-de": 1031, "fr-fr": 1036, "es-es": 3082,
    "it-it": 1040, "pt-br": 1046, "pt-pt": 2070, "nl-nl": 1043, "pl-pl": 1045,
    "ru-ru": 1049, "ja-jp": 1041, "zh-cn": 2052, "zh-tw": 1028, "ko-kr": 1042,
    "sv-se": 1053, "da-dk": 1030, "nb-no": 1044, "fi-fi": 1035, "tr-tr": 1055,
    "cs-cz": 1029, "hu-hu": 1038, "uk-ua": 1058, "ar-sa": 1025, "he-il": 1037,
    "th-th": 1054,
}


def latest(channel):
    # The release service rejects urllib's default user agent; curl is fine.
    out = subprocess.run(["curl", "-fsSL", RELEASES], check=True, capture_output=True).stdout
    for c in json.loads(out):
        if c["channelId"] == channel:
            return c["latestVersion"]
    raise SystemExit("unknown channel: %s" % channel)


def fetch(url, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    # -C - resumes interrupted downloads; size check below catches truncation.
    subprocess.run(["curl", "-fsSL", "--retry", "5", "-C", "-", "-o", dest, url], check=True)
    return dest


def main():
    dest, channel, arch, *langs = sys.argv[1:]
    bits = "64" if arch in ("64", "x64") else "32"
    xarch = "x64" if bits == "64" else "x86"
    ver = latest(channel)
    base = "http://officecdn.microsoft.com/pr/%s/Office/Data" % CHANNELS[channel]
    out = os.path.join(dest, "Office", "Data")

    files = ["v%s.cab" % bits, "v%s_%s.cab" % (bits, ver)]
    vfiles = ["i%s0.cab" % bits, "s%s0.cab" % bits, "stream.%s.x-none.dat" % xarch]
    for lang in langs:
        lcid = LCIDS[lang.lower()]
        vfiles += ["i%s%d.cab" % (bits, lcid), "s%s%d.cab" % (bits, lcid), "stream.%s.%s.dat" % (xarch, lang.lower())]
    jobs = [(base + "/" + f, os.path.join(out, f)) for f in files]
    jobs += [("%s/%s/%s" % (base, ver, f), os.path.join(out, ver, f)) for f in vfiles]

    with ThreadPoolExecutor(max_workers=8) as pool:
        for path in pool.map(lambda j: fetch(*j), jobs):
            print("  " + os.path.relpath(path, dest), file=sys.stderr)
    print(ver)


if __name__ == "__main__":
    main()
