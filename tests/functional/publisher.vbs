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

' Repeated edits exercise the document undo stack as well as text persistence.
' The text engine's interactive undo storage has separate coverage in ole32's
' test_delete_on_release_stat; ordinary automation alone did not expose it.
Dim i, initialShapes, stressText, stressName
initialShapes = page2.Shapes.Count
For i = 1 To 25
  stressText = "Publisher textbox stress " & i & vbCrLf & String(200, "x")
  stressName = "TuxOffice stress " & i
  doc.BeginCustomUndoAction "Textbox stress " & i
  Set box = page2.Shapes.AddTextbox(1, 30 + (i Mod 5) * 100, 30 + Int((i - 1) / 5) * 100, 90, 90)
  box.Name = stressName
  box.TextFrame.TextRange.Text = stressText
  box.TextFrame.TextRange.Font.Size = 8 + (i Mod 5)
  box.TextFrame.TextRange.Font.Bold = (i Mod 2 = 0)
  doc.EndCustomUndoAction
  checked = Empty : checked = (page2.Shapes.Count = initialShapes + i) : Check "stress add " & i, checked
  Set box = Nothing
  doc.Undo
  checked = Empty : checked = (page2.Shapes.Count = initialShapes + i - 1) : Check "stress undo " & i, checked
  doc.Redo
  checked = Empty : checked = (page2.Shapes.Count = initialShapes + i) : Check "stress redo " & i, checked
  Set box = page2.Shapes(stressName)
  checked = Empty : checked = (InStr(box.TextFrame.TextRange.Text, "Publisher textbox stress " & i) = 1) : Check "stress text " & i, checked
Next
WScript.Sleep 1500

Dim pub, pdf, xps
pub = dir & "\publisher-test.pub" : pdf = dir & "\publisher-test.pdf"
Remove pub : Remove pdf
doc.SaveAs pub
checked = Empty : checked = (SizeOf(pub) > 1000) : Check "save pub", checked
doc.ExportAsFixedFormat 2, pdf
checked = Empty : checked = (SizeOf(pdf) > 500) : Check "export pdf", checked
Set box = Nothing : Set shape = Nothing : Set tbl = Nothing
Set page = Nothing : Set page2 = Nothing
doc.Close
Set doc = Nothing
Err.Clear
Set doc = app.Open(pub)
checked = Empty : checked = (doc.Pages.Count = 2) : Check "reopen", checked
checked = Empty : checked = (PageHasText(doc.Pages(1), "TuxOffice")) : Check "reopen keeps text", checked
checked = Empty : checked = (doc.Pages(2).Shapes.Count = initialShapes + 25) : Check "reopen keeps stress boxes", checked
For i = 1 To 25
  Set box = doc.Pages(2).Shapes("TuxOffice stress " & i)
  checked = Empty : checked = (InStr(box.TextFrame.TextRange.Text, "Publisher textbox stress " & i) = 1) : Check "reopen stress text " & i, checked
Next
Set box = Nothing
doc.Close
Set doc = Nothing
app.Quit
checked = Empty : checked = (Err.Number = 0) : Check "quit", checked
Set app = Nothing
WScript.Echo "RESULT publisher passed=" & passes & " failed=" & fails
If fails > 0 Then WScript.Quit 1
