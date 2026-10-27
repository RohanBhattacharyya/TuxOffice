#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write the Word and Excel documents used by the scrolling benchmarks.

Usage: scroll-fixtures.py OUTDIR [PARAGRAPHS=1000] [ROWS=50000]
Creates OUTDIR/scroll-word.docx and OUTDIR/scroll-excel.xlsx. The PowerPoint
fixture comes from presentation-fixture.py and the suite's presentation.
"""
from pathlib import Path
import sys
from xml.sax.saxutils import escape
from zipfile import ZipFile, ZIP_DEFLATED

out = Path(sys.argv[1])
paragraphs = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
rows = int(sys.argv[3]) if len(sys.argv) > 3 else 50000
out.mkdir(parents=True, exist_ok=True)
types = ('<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
         '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
         '<Default Extension="xml" ContentType="application/xml"/>')
rels = '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
text = ('Paragraph {} of the TuxOffice scrolling fixture. Lorem ipsum dolor sit amet, '
        'consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.')

with ZipFile(out / 'scroll-word.docx', 'w', ZIP_DEFLATED) as archive:
    archive.writestr('[Content_Types].xml', types + '<Override PartName="/word/document.xml" '
        'ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
    archive.writestr('_rels/.rels', rels + '<Relationship Id="r1" Type="http://schemas.openxmlformats.org/'
        'officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
    body = ''.join(f'<w:p><w:r><w:t>{escape(text.format(i + 1))}</w:t></w:r></w:p>' for i in range(paragraphs))
    archive.writestr('word/document.xml', '<w:document xmlns:w="http://schemas.openxmlformats.org/'
        'wordprocessingml/2006/main"><w:body>' + body + '</w:body></w:document>')

with ZipFile(out / 'scroll-excel.xlsx', 'w', ZIP_DEFLATED) as archive:
    archive.writestr('[Content_Types].xml', types + '<Override PartName="/xl/workbook.xml" ContentType='
        '"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName='
        '"/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.'
        'spreadsheetml.worksheet+xml"/></Types>')
    archive.writestr('_rels/.rels', rels + '<Relationship Id="r1" Type="http://schemas.openxmlformats.org/'
        'officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>')
    archive.writestr('xl/workbook.xml', '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
        'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>'
        '<sheet name="Rows" sheetId="1" r:id="r1"/></sheets></workbook>')
    archive.writestr('xl/_rels/workbook.xml.rels', rels + '<Relationship Id="r1" Type="http://schemas.'
        'openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
        '</Relationships>')
    data = ''.join(f'<row r="{r}"><c r="A{r}"><v>{r}</v></c><c r="B{r}" t="inlineStr"><is><t>Row {r}</t></is></c>'
                   f'<c r="C{r}"><v>{r * 3 % 997}</v></c></row>' for r in range(1, rows + 1))
    archive.writestr('xl/worksheets/sheet1.xml', '<worksheet xmlns="http://schemas.openxmlformats.org/'
        'spreadsheetml/2006/main"><sheetData>' + data + '</sheetData></worksheet>')
print(out / 'scroll-word.docx', out / 'scroll-excel.xlsx')
