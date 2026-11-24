#!/usr/bin/env python3
"""Open a Single File Web Page (.mht) in the default Linux browser.

Office saves a mail or document as MHT for "View in Browser" and asks the shell
to open it. On Windows the browser reads MHT itself; Linux browsers do not. Unpack
the archive into a private directory as an HTML page and its parts, pointing the
page's references at the unpacked parts, and open the page.

The content comes from mail, so the page gets a policy that blocks scripts,
plugins and form submission. File names are generated here, never taken from
the archive.

usage: webarchive.py PREFIX PATH
PATH is a Windows path (C:\\...) inside PREFIX, or a Unix path.
"""
import email
import email.policy
import html
import mimetypes
import os
import re
import secrets
import shutil
import subprocess
import sys
import time

POLICY = ("default-src 'none'; img-src * data: blob: file:; media-src * data: file:; "
          "style-src 'unsafe-inline' * file:; font-src * data: file:; form-action 'none'")


def unix_path(prefix, path):
    """Maps a Windows drive path to the prefix's dosdevices links."""
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", path)
    if not match:
        return path
    path = os.path.join(prefix, "dosdevices", match.group(1).lower() + ":")
    # Windows paths ignore case; match each component the way Wine does.
    for component in (p for p in re.split(r"[\\/]+", match.group(2)) if p):
        candidate = os.path.join(path, component)
        if not os.path.exists(candidate) and os.path.isdir(path):
            folded = component.casefold()
            candidate = next((os.path.join(path, entry) for entry in os.listdir(path)
                              if entry.casefold() == folded), candidate)
        path = candidate
    return path


def root_part(message):
    """The part the archive starts at: multipart/related's start, or the first HTML part."""
    parts = [p for p in message.walk() if not p.is_multipart()]
    start = message.get_param("start") if message.is_multipart() else None
    if start:
        for part in parts:
            if part.get("Content-ID", "").strip() == start.strip():
                return part, parts
    for part in parts:
        if part.get_content_type() in ("text/html", "application/xhtml+xml"):
            return part, parts
    return (parts[0] if parts else None), parts


def html_text(part):
    try:
        return part.get_content()
    except (LookupError, ValueError):
        payload = part.get_payload(decode=True) or b""
        return payload.decode("utf-8", "replace")


def unpack(path, out):
    with open(path, "rb") as f:
        message = email.message_from_binary_file(f, policy=email.policy.default)
    root, parts = root_part(message)
    if root is None:
        raise ValueError("no content in the archive")

    references = {}
    for index, part in enumerate(parts):
        if part is root:
            continue
        extension = mimetypes.guess_extension(part.get_content_type()) or ".bin"
        name = "part%d%s" % (index, extension)
        with open(os.path.join(out, name), "wb") as f:
            f.write(part.get_payload(decode=True) or b"")
        location = (part.get("Content-Location") or "").strip()
        if location:
            references[location] = name
            # Office writes absolute file:/// locations and refers to them relatively.
            base = re.split(r"[\\/]", location)[-1]
            if base and base != location:
                references.setdefault(base, name)
        cid = (part.get("Content-ID") or "").strip().strip("<>")
        if cid:
            references["cid:" + cid] = name

    page = html_text(root)
    if references:
        pattern = re.compile("|".join(re.escape(k) for k in sorted(references, key=len, reverse=True)))
        page = pattern.sub(lambda m: references[m.group(0)], page)
        # Locations may also appear HTML-escaped inside attributes.
        escaped = {html.escape(k, quote=True): v for k, v in references.items() if html.escape(k, quote=True) != k}
        if escaped:
            pattern = re.compile("|".join(re.escape(k) for k in sorted(escaped, key=len, reverse=True)))
            page = pattern.sub(lambda m: escaped[m.group(0)], page)

    head = ('<meta charset="utf-8">'
            '<meta http-equiv="Content-Security-Policy" content="%s">' % html.escape(POLICY, quote=False))
    match = re.search(r"<head[^>]*>", page, re.IGNORECASE)
    if match:
        page = page[:match.end()] + head + page[match.end():]
    else:
        page = head + page
    index = os.path.join(out, "index.html")
    with open(index, "w", encoding="utf-8") as f:
        f.write(page)
    return index


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    path = unix_path(sys.argv[1], sys.argv[2])
    base = os.environ.get("XDG_RUNTIME_DIR") or os.path.join(os.path.expanduser("~"), ".cache")
    root = os.path.join(base, "tuxoffice-web-archives")
    os.makedirs(root, mode=0o700, exist_ok=True)
    # The browser may still be reading recent pages; drop the ones from earlier days.
    for entry in os.scandir(root):
        if entry.is_dir(follow_symlinks=False) and time.time() - entry.stat().st_mtime > 86400:
            shutil.rmtree(entry.path, ignore_errors=True)
    out = os.path.join(root, secrets.token_hex(8))
    os.mkdir(out, 0o700)
    index = unpack(path, out)
    if os.environ.get("TUXOFFICE_WEBARCHIVE_NO_OPEN"):
        print(index)
        return 0
    return subprocess.call(["xdg-open", "file://" + index])


if __name__ == "__main__":
    sys.exit(main())
