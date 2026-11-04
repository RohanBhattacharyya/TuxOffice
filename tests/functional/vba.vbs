' SPDX-License-Identifier: LGPL-2.1-or-later
' An Excel VBA macro drives Word with early binding (a reference to Word's type
' library), so its calls go through proxies built from that 32-bit type
' library. Those were broken until rpcrt4 counted vtable slots in the unit
' oleaut32 reports, and the macro never returned.
' Usage: cscript //nologo vba.vbs C:\work\dir
Option Explicit
Dim fails, passes, sh, xl, wb, vbp, m, code, result
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

' "Trust access to the VBA project object model", for this test prefix.
Set sh = CreateObject("WScript.Shell")
sh.RegWrite "HKCU\Software\Microsoft\Office\16.0\Excel\Security\AccessVBOM", 1, "REG_DWORD"
Err.Clear

Set xl = CreateObject("Excel.Application") : xl.DisplayAlerts = False
Set wb = xl.Workbooks.Add
Set vbp = wb.VBProject
Check "VBA project", Not vbp Is Nothing
vbp.References.AddFromGuid "{00020905-0000-0000-C000-000000000046}", 8, 0
Check "reference to Word's type library", True
Set m = vbp.VBComponents.Add(1)
code = "Function EarlyBound() As String" & vbLf & _
  "  Dim wd As Word.Application, doc As Word.Document" & vbLf & _
  "  Set wd = New Word.Application" & vbLf & _
  "  Set doc = wd.Documents.Add" & vbLf & _
  "  doc.Content.Text = ""Early bound from Excel VBA""" & vbLf & _
  "  doc.Paragraphs(1).Range.Font.Bold = True" & vbLf & _
  "  EarlyBound = doc.Words.Count & "" "" & doc.Paragraphs(1).Range.Font.Bold" & vbLf & _
  "  doc.Close False" & vbLf & _
  "  wd.Quit" & vbLf & _
  "End Function"
m.CodeModule.AddFromString code
Check "macro added", m.CodeModule.CountOfLines > 5
result = xl.Run("EarlyBound")
Check "early-bound calls into Word return (" & result & ")", result = "6 -1"
wb.Close False
xl.Quit
Check "excel quits", True
WScript.Echo "RESULT vba passed=" & passes & " failed=" & fails
