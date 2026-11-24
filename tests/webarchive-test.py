#!/usr/bin/env python3
"""Checks lib/webarchive.py on an archive shaped like Outlook's "View in Browser" MHT."""
import base64
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "lib", "webarchive.py")
PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==")

ARCHIVE = (
    "MIME-Version: 1.0\r\n"
    'Content-Type: multipart/related; boundary="----=_NextPart_01"\r\n'
    "\r\n"
    "This document is a Single File Web Page, also known as a Web Archive file.\r\n"
    "\r\n"
    "------=_NextPart_01\r\n"
    "Content-Location: file:///C:/D1B2/email.htm\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n"
    'Content-Type: text/html; charset="windows-1252"\r\n'
    "\r\n"
    "<html><head><meta http-equiv=3DContent-Type content=3D\"text/html; charset=3Dwindows-1252\">"
    "</head><body><p>Caf=E9</p>"
    '<img src=3D"email_files/image001.png"><img src=3D"cid:logo@01">'
    '<img src=3D"https://example.com/remote.png"><script>alert(1)</script></body></html>\r\n'
    "\r\n"
    "------=_NextPart_01\r\n"
    "Content-Location: file:///C:/D1B2/email_files/image001.png\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "Content-Type: image/png\r\n"
    "\r\n" + base64.encodebytes(PNG).decode().replace("\n", "\r\n") + "\r\n"
    "------=_NextPart_01\r\n"
    "Content-ID: <logo@01>\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "Content-Type: image/png\r\n"
    "\r\n" + base64.encodebytes(PNG).decode().replace("\n", "\r\n") + "\r\n"
    "------=_NextPart_01--\r\n"
)

failures = 0


def check(ok, what):
    global failures
    print(("PASS " if ok else "FAIL ") + what)
    failures += not ok


with tempfile.TemporaryDirectory() as work:
    prefix = os.path.join(work, "prefix")
    folder = os.path.join(prefix, "drive_c", "users", "me", "AppData", "Local", "Temp")
    os.makedirs(folder)
    os.makedirs(os.path.join(prefix, "dosdevices"))
    os.symlink("../drive_c", os.path.join(prefix, "dosdevices", "c:"))
    with open(os.path.join(folder, "email.mht"), "w", newline="") as f:
        f.write(ARCHIVE)
    env = dict(os.environ, TUXOFFICE_WEBARCHIVE_NO_OPEN="1", XDG_RUNTIME_DIR=work)
    # Windows paths ignore case.
    result = subprocess.run([sys.executable, "-I", TOOL, prefix, r"C:\USERS\me\appdata\local\temp\email.mht"],
                            env=env, capture_output=True, text=True)
    check(result.returncode == 0, "unpacks the archive (%s)" % result.stderr.strip()[-200:])
    index = result.stdout.strip()
    page = open(index, encoding="utf-8").read() if os.path.isfile(index) else ""
    out = os.path.dirname(index)
    check(page.index("Content-Security-Policy") < page.index("<p>"), "policy is in the head, before the content")
    check("script-src" not in page and "default-src 'none'" in page, "scripts are blocked by default-src")
    check("Café" in page, "windows-1252 text is decoded")
    check(page.find('<meta charset="utf-8">') < page.find("charset=windows-1252"), "utf-8 charset comes first")
    local = [name for name in os.listdir(out) if name.startswith("part")]
    check(len(local) == 2, "both images are unpacked (%s)" % local)
    check("email_files/image001.png" not in page and "cid:logo@01" not in page, "references point at the unpacked parts")
    check(all(name in page for name in local), "the page uses the unpacked parts")
    check("https://example.com/remote.png" in page, "remote images are left for the browser to load")
    check(all(open(os.path.join(out, name), "rb").read() == PNG for name in local), "image data is intact")
    check(oct(os.stat(out).st_mode & 0o777) == "0o700", "the unpacked folder is private")

print("%d failures" % failures)
sys.exit(1 if failures else 0)
