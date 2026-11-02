' SPDX-License-Identifier: LGPL-2.1-or-later
' Publisher object-model test: pages, shapes, text, tables, images, save, PDF.
' Usage: cscript //nologo publisher.vbs C:\work\dir
Option Explicit
Dim checked
Dim dir, app, doc, fails, passes
dir = WScript.Arguments(0)
fails = 0 : passes = 0

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

Function SizeOf(path)
  Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
  SizeOf = 0
  If fso.FileExists(path) Then SizeOf = fso.GetFile(path).Size
End Function

Sub Remove(path)
  Dim fso : Set fso = CreateObject("Scripting.FileSystemObject")
  If fso.FileExists(path) Then fso.DeleteFile path
End Sub

Function PageHasText(page, wanted)
  Dim shape, i
  PageHasText = False
  For i = 1 To page.Shapes.Count
    Set shape = page.Shapes(i)
    If shape.HasTextFrame Then
      If InStr(shape.TextFrame.TextRange.Text, wanted) > 0 Then PageHasText = True
    End If
  Next
End Function

On Error Resume Next
Set app = CreateObject("Publisher.Application")
checked = Empty : checked = (Not app Is Nothing) : Check "create Publisher.Application", checked
Set doc = app.NewDocument()
checked = Empty : checked = (Not doc Is Nothing) : Check "new document", checked
app.ActiveWindow.Visible = True
checked = Empty : checked = (app.ActiveWindow.Visible) : Check "visible", checked
checked = Empty : checked = (doc.Pages.Count = 1) : Check "one page", checked

Dim page, box, shape, tbl
Set page = doc.Pages(1)
Set box = page.Shapes.AddTextbox(1, 72, 72, 400, 120)
checked = Empty : checked = (Not box Is Nothing) : Check "add text box", checked
box.TextFrame.TextRange.Text = "TuxOffice Publisher test"
checked = Empty : checked = (InStr(box.TextFrame.TextRange.Text, "TuxOffice") = 1) : Check "set text", checked
box.TextFrame.TextRange.Font.Size = 28
box.TextFrame.TextRange.Font.Bold = True
checked = Empty : checked = (box.TextFrame.TextRange.Font.Size = 28) : Check "format text", checked
Set shape = page.Shapes.AddShape(9, 72, 220, 180, 120)
checked = Empty : checked = (Not shape Is Nothing) : Check "add shape", checked
shape.Fill.ForeColor.RGB = RGB(0, 120, 215)
checked = Empty : checked = (shape.Fill.ForeColor.RGB = RGB(0, 120, 215)) : Check "fill shape", checked
Set tbl = page.Shapes.AddTable(3, 3, 280, 220, 240, 120)
checked = Empty : checked = (tbl.Table.Rows.Count = 3) : Check "add table", checked
tbl.Table.Rows(1).Cells(1).TextRange.Text = "Cell"
checked = Empty : checked = (InStr(tbl.Table.Rows(1).Cells(1).TextRange.Text, "Cell") = 1) : Check "table text", checked
Dim page2 : Set page2 = doc.Pages.Add(1, 1)
checked = Empty : checked = (doc.Pages.Count = 2) : Check "add page", checked
WScript.Sleep 1500

Dim pub, pdf, xps
pub = dir & "\publisher-test.pub" : pdf = dir & "\publisher-test.pdf"
Remove pub : Remove pdf
doc.SaveAs pub
checked = Empty : checked = (SizeOf(pub) > 1000) : Check "save pub", checked
doc.ExportAsFixedFormat 2, pdf
checked = Empty : checked = (SizeOf(pdf) > 500) : Check "export pdf", checked
doc.Close
Err.Clear
Set doc = app.Open(pub)
checked = Empty : checked = (doc.Pages.Count = 2) : Check "reopen", checked
checked = Empty : checked = (PageHasText(doc.Pages(1), "TuxOffice")) : Check "reopen keeps text", checked
doc.Close
app.Quit
checked = Empty : checked = (Err.Number = 0) : Check "quit", checked
WScript.Echo "RESULT publisher passed=" & passes & " failed=" & fails
