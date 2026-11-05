' SPDX-License-Identifier: LGPL-2.1-or-later
' Excel's Power Query: import a CSV through a query and load it into a table.
' Its engine is .NET, so this needs Microsoft's .NET Framework 4.8 in the
' prefix (setup installs it); on wine-mono the add-in fails to start.
' Usage: cscript //nologo powerquery.vbs C:\work\dir
Option Explicit
Dim dir, fails, passes, fso, f, xl, wb, ws, q, lo, m
dir = WScript.Arguments(0) & "\powerquery"
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

Set fso = CreateObject("Scripting.FileSystemObject")
If fso.FolderExists(dir) Then fso.DeleteFolder dir, True
fso.CreateFolder dir
Set f = fso.CreateTextFile(dir & "\data.csv", True)
f.Write "Name,City,Amount" & vbCrLf & "Ada,London,10" & vbCrLf & "Linus,Helsinki,20" & vbCrLf & "Grace,Arlington,30" & vbCrLf
f.Close

Set xl = CreateObject("Excel.Application") : xl.DisplayAlerts = False
Set wb = xl.Workbooks.Add : Set ws = wb.Worksheets(1)
m = "let Source = Csv.Document(File.Contents(""" & dir & "\data.csv""), [Delimiter="","", Encoding=65001])," & _
    " Promoted = Table.PromoteHeaders(Source)," & _
    " Typed = Table.TransformColumnTypes(Promoted, {{""Amount"", Int64.Type}})," & _
    " Filtered = Table.SelectRows(Typed, each [Amount] > 15) in Filtered"
Set q = wb.Queries.Add("Data", m)
Check "query added", wb.Queries.Count = 1
Set lo = ws.ListObjects.Add(0, "OLEDB;Provider=Microsoft.Mashup.OleDb.1;Data Source=$Workbook$;Location=Data", , 1, ws.Range("A1"))
lo.QueryTable.CommandType = 2
lo.QueryTable.CommandText = Array("SELECT * FROM [Data]")
lo.QueryTable.Refresh False
Check "query refreshes", True
Check "headers loaded (" & ws.Range("A1").Value & ")", ws.Range("A1").Value = "Name"
Check "filter applied (" & ws.Range("A2").Value & ")", ws.Range("A2").Value = "Linus"
Check "typed numbers (" & ws.Range("C3").Value & ")", ws.Range("C3").Value = 30 And VarType(ws.Range("C3").Value) = 5
Check "row count (" & lo.ListRows.Count & ")", lo.ListRows.Count = 2
wb.SaveAs dir & "\query.xlsx", 51
Check "workbook with query saves", fso.FileExists(dir & "\query.xlsx")
wb.Close False
xl.Quit
Check "excel quits", True
WScript.Echo "RESULT powerquery passed=" & passes & " failed=" & fails
