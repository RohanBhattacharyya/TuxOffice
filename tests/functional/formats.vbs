' SPDX-License-Identifier: LGPL-2.1-or-later
' Save in other file formats and open the results again: Word (doc, rtf, htm, mht,
' odt, pdf, txt), Excel (xls, csv, ods, xml, htm, mht, pdf) and PowerPoint (ppt,
' odp, pdf). Word opening a PDF converts it through Office's IConverter, which
' crashed in the converter until rpcrt4 counted vtable slots correctly.
' Usage: cscript //nologo formats.vbs C:\work\dir
Option Explicit
Dim dir, fails, passes, k, wd, doc, d2, xl, wb, ws, w2, pp, pres, sl, p2, i, e
Dim wf, we, xf, xe, pf, pe, sh
dir = WScript.Arguments(0) & "\formats"
fails = 0 : passes = 0
On Error Resume Next

Sub Check(name, ok)
  If Err.Number <> 0 Then
    WScript.Echo "FAIL " & name & " err=" & Hex(Err.Number) & " " & Err.Description
    Err.Clear : fails = fails + 1
  ElseIf ok Then
    WScript.Echo "PASS " & name : passes = passes + 1
  Else
    WScript.Echo "FAIL " & name : fails = fails + 1
  End If
End Sub

Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
If fso.FolderExists(dir) Then fso.DeleteFolder dir, True
fso.CreateFolder dir

' Word asks before converting a PDF; this is its "Don't show this message again".
Set sh = CreateObject("WScript.Shell")
sh.RegWrite "HKCU\Software\Microsoft\Office\16.0\Word\Options\DisableConvertPdfWarning", 1, "REG_DWORD"
Err.Clear

Set wd = CreateObject("Word.Application") : wd.DisplayAlerts = 0
Set doc = wd.Documents.Add
doc.Content.Text = "TuxOffice format round trip." & vbCr & "Second paragraph."
doc.Paragraphs(2).Range.Font.Bold = True
wf = Array(0, 6, 8, 23, 17, 2, 9)
we = Array("doc", "rtf", "htm", "odt", "pdf", "txt", "mht")
For k = 0 To UBound(wf)
  doc.SaveAs2 dir & "\word." & we(k), wf(k) : Check "word saves " & we(k), fso.FileExists(dir & "\word." & we(k))
Next
doc.Close False
For k = 0 To UBound(we)
  Set d2 = Nothing
  Set d2 = wd.Documents.Open(dir & "\word." & we(k), False, True)
  Check "word opens " & we(k), Not d2 Is Nothing
  If Not d2 Is Nothing Then
    Check "word keeps the text of " & we(k), InStr(d2.Content.Text, "format round trip") > 0
    d2.Close False
  End If
Next
wd.Quit : Check "word quits", True

Set xl = CreateObject("Excel.Application") : xl.DisplayAlerts = False
Set wb = xl.Workbooks.Add : Set ws = wb.Worksheets(1)
For i = 1 To 5 : ws.Cells(i, 1).Value = i : ws.Cells(i, 2).Formula = "=A" & i & "*2" : Next
xf = Array(56, 6, 60, 46, 44, 45)
xe = Array("xls", "csv", "ods", "xml", "htm", "mht")
For k = 0 To UBound(xf)
  wb.SaveAs dir & "\excel." & xe(k), xf(k) : Check "excel saves " & xe(k), fso.FileExists(dir & "\excel." & xe(k))
Next
wb.ExportAsFixedFormat 0, dir & "\excel.pdf" : Check "excel exports pdf", fso.FileExists(dir & "\excel.pdf")
wb.Close False
For k = 0 To UBound(xe)
  Set w2 = Nothing
  Set w2 = xl.Workbooks.Open(dir & "\excel." & xe(k))
  Check "excel opens " & xe(k), Not w2 Is Nothing
  If Not w2 Is Nothing Then
    Check "excel keeps the values of " & xe(k), w2.Worksheets(1).Range("B5").Value = 10
    w2.Close False
  End If
Next
xl.Quit : Check "excel quits", True

Set pp = CreateObject("PowerPoint.Application")
Set pres = pp.Presentations.Add(False)
Set sl = pres.Slides.Add(1, 2)
sl.Shapes(1).TextFrame.TextRange.Text = "Round trip"
sl.Shapes(2).TextFrame.TextRange.Text = "Body text"
pf = Array(1, 35, 32)
pe = Array("ppt", "odp", "pdf")
For k = 0 To UBound(pf)
  pres.SaveAs dir & "\powerpoint." & pe(k), pf(k) : Check "powerpoint saves " & pe(k), fso.FileExists(dir & "\powerpoint." & pe(k))
Next
pres.Close
For Each e In Array("ppt", "odp")
  Set p2 = Nothing
  Set p2 = pp.Presentations.Open(dir & "\powerpoint." & e, True, False, False)
  Check "powerpoint opens " & e, Not p2 Is Nothing
  If Not p2 Is Nothing Then
    Check "powerpoint keeps the slide of " & e, p2.Slides(1).Shapes(1).TextFrame.TextRange.Text = "Round trip"
    p2.Close
  End If
Next
pp.Quit : Check "powerpoint quits", True
WScript.Echo "RESULT formats passed=" & passes & " failed=" & fails
