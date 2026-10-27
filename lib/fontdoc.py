# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write a .docx that references every font Office should fetch from its
cloud font service (UI fonts and the default document fonts).

Usage: fontdoc.py OUT.docx

Word downloads a cloud font when a document uses it; opening this file once
(see lib/cloudfonts.vbs) primes Office's font cache, which TuxOffice then
registers prefix-wide. Standard library only.
"""
import sys
import zipfile
from xml.sax.saxutils import escape

# (family, bold, italic) for each face Office's UI and templates use.
FACES = [(f, b, i) for f in ("Segoe UI", "Calibri", "Cambria", "Consolas", "Candara",
                             "Constantia", "Corbel", "Aptos", "Aptos Display", "Aptos Narrow",
                             "Aptos Mono")
         for b in (False, True) for i in (False, True)]
FACES += [(f, False, False) for f in ("Segoe UI Light", "Segoe UI Semilight", "Segoe UI Semibold",
                                      "Segoe UI Black", "Segoe UI Symbol", "Segoe UI Emoji",
                                      "Calibri Light", "Aptos Light", "Aptos SemiBold",
                                      "Aptos ExtraBold", "Aptos Black")]

SAMPLE = "The quick brown fox jumps over the lazy dog 0123456789 → ✓ ☺"


def run(family, bold, italic):
    props = '<w:rFonts w:ascii="{0}" w:hAnsi="{0}" w:cs="{0}" w:eastAsia="{0}"/>'.format(escape(family, {'"': "&quot;"}))
    props += "<w:b/>" if bold else ""
    props += "<w:i/>" if italic else ""
    return '<w:p><w:r><w:rPr>%s</w:rPr><w:t xml:space="preserve">%s: %s</w:t></w:r></w:p>' % (
        props, escape(family), escape(SAMPLE))


def main(out):
    body = "".join(run(*face) for face in FACES)
    document = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
                "<w:body>%s</w:body></w:document>" % body)
    types = ('<?xml version="1.0" encoding="UTF-8"?>'
             '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="xml" ContentType="application/xml"/>'
             '<Override PartName="/word/document.xml" '
             'ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
             "</Types>")
    rels = ('<?xml version="1.0" encoding="UTF-8"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Id="rId1" '
            'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" '
            'Target="word/document.xml"/></Relationships>')
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", types)
        z.writestr("_rels/.rels", rels)
        z.writestr("word/document.xml", document)


if __name__ == "__main__":
    main(sys.argv[1])
