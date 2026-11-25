' SPDX-License-Identifier: LGPL-2.1-or-later
' Access object-model test: tables, SQL, queries, forms, reports, VBA, export.
' Usage: cscript //nologo access.vbs C:\work\dir
Option Explicit
Dim checked
Dim dir, app, db, rs, fails, passes
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

Function Exists(path)
  Exists = CreateObject("Scripting.FileSystemObject").FileExists(path)
End Function

Function SizeOf(path)
  SizeOf = 0
  If Exists(path) Then SizeOf = CreateObject("Scripting.FileSystemObject").GetFile(path).Size
End Function

Function Scalar(sql)
  Dim r : Set r = db.OpenRecordset(sql)
  Scalar = r.Fields(0).Value
  r.Close
End Function

Function QueryValue(name, column)
  Dim q, r
  Set q = db.QueryDefs(name)
  Set r = q.OpenRecordset()
  QueryValue = r.Fields(column).Value
  r.Close
End Function

Function AdoCount(connection)
  Dim r : Set r = connection.Execute("SELECT COUNT(*) FROM Items")
  AdoCount = r.Fields(0).Value
  r.Close
End Function

Sub Remove(path)
  If Exists(path) Then CreateObject("Scripting.FileSystemObject").DeleteFile path
End Sub

On Error Resume Next
Dim dbPath : dbPath = dir & "\access-test.accdb"
Remove dbPath
Set app = CreateObject("Access.Application")
checked = Empty : checked = (Not app Is Nothing) : Check "create Access.Application", checked
app.Visible = True
checked = Empty : checked = (app.Visible) : Check "make visible", checked
app.NewCurrentDatabase dbPath
checked = Empty : checked = (Exists(dbPath)) : Check "create accdb", checked
Set db = app.CurrentDb
checked = Empty : checked = (Not db Is Nothing) : Check "CurrentDb", checked

db.Execute "CREATE TABLE Items (Id AUTOINCREMENT PRIMARY KEY, Name TEXT(50), Qty LONG, Price CURRENCY, Added DATETIME, Remark MEMO)", 128
Check "run CREATE TABLE", True
db.TableDefs.Refresh
checked = Empty : checked = (db.TableDefs("Items").Fields.Count = 6) : Check "CREATE TABLE", checked

Dim i, started : started = Timer
Set rs = db.OpenRecordset("Items")
For i = 1 To 2000
  rs.AddNew
  rs.Fields("Name").Value = "Item " & i
  rs.Fields("Qty").Value = i
  rs.Fields("Price").Value = i * 1.5
  rs.Fields("Added").Value = Now
  rs.Fields("Remark").Value = "Long text field for row " & i
  rs.Update
Next
rs.Close
WScript.Echo "TIME insert2000 " & FormatNumber(Timer - started, 3)
checked = Empty : checked = (Scalar("SELECT COUNT(*) FROM Items") = 2000) : Check "insert rows", checked

checked = Empty : checked = (Scalar("SELECT SUM(Qty) FROM Items") = 2001000) : Check "SELECT SUM", checked
db.Execute "UPDATE Items SET Qty = Qty * 2 WHERE Id <= 10", 128
checked = Empty : checked = (Scalar("SELECT SUM(Qty) FROM Items WHERE Id <= 10") = 110) : Check "UPDATE", checked
db.Execute "DELETE FROM Items WHERE Id > 1500", 128
checked = Empty : checked = (Scalar("SELECT COUNT(*) FROM Items") >= 1500) : Check "DELETE", checked
checked = Empty : checked = (app.DLookup("Name", "Items", "Id=7") = "Item 7") : Check "DLookup", checked
checked = Empty : checked = (app.DSum("Qty", "Items", "Id=1") = 2) : Check "DSum", checked
checked = Empty : checked = (app.Eval("1+2") = 3) : Check "Eval", checked

db.CreateQueryDef "BigItems", "SELECT Name, Qty FROM Items WHERE Qty > 1000 ORDER BY Qty DESC"
checked = Empty : checked = (QueryValue("BigItems", 1) = 1500) : Check "saved query", checked
app.DoCmd.OpenQuery "BigItems"
checked = Empty : checked = (app.Screen.ActiveDatasheet.Name = "BigItems") : Check "open query datasheet", checked
app.DoCmd.Close 1, "BigItems"
Err.Clear

app.DoCmd.OpenTable "Items"
checked = Empty : checked = (app.Screen.ActiveDatasheet.Name = "Items") : Check "open table datasheet", checked
WScript.Sleep 800
app.DoCmd.Close 0, "Items"
Err.Clear

' A form bound to the table with a text box
Dim frm, ctl, frmName
Set frm = app.CreateForm()
checked = Empty : checked = (Not frm Is Nothing) : Check "CreateForm", checked
frm.RecordSource = "Items"
Set ctl = app.CreateControl(frm.Name, 109, 0, "", "Name", 1000, 500, 3000, 400)
checked = Empty : checked = (Not ctl Is Nothing) : Check "CreateControl", checked
ctl.Name = "txtName"
frmName = frm.Name
app.DoCmd.Save 2, frmName
app.DoCmd.Close 2, frmName, 1
checked = Empty : checked = (Err.Number = 0) : Check "save form", checked
app.DoCmd.OpenForm frmName
checked = Empty : checked = (app.Forms(frmName).Controls("txtName").Value = "Item 1") : Check "open form", checked
' acForm, acNext, five records on
app.DoCmd.GoToRecord 2, frmName, 1, 5
checked = Empty : checked = (app.Forms(frmName).Controls("txtName").Value = "Item 6") : Check "navigate form", checked
WScript.Sleep 800
app.DoCmd.Close 2, frmName
Err.Clear

' Code behind a form, then deleted: Access builds these type libraries at run time
Dim cbf, cbfName
Set cbf = app.CreateForm()
cbfName = cbf.Name
cbf.HasModule = True
cbf.Module.InsertText "Public Function TuxValue()" & vbCrLf & "TuxValue = 7" & vbCrLf & "End Function" & vbCrLf
cbf.Module.InsertText "Private Sub Form_Open(Cancel As Integer)" & vbCrLf & "Me.Tag = ""onopen passed""" & vbCrLf & "End Sub" & vbCrLf
cbf.OnOpen = "[Event Procedure]"
app.DoCmd.Close 2, cbfName, 1
checked = Empty : checked = (Err.Number = 0) : Check "save form with code", checked
app.DoCmd.OpenForm cbfName
checked = Empty : checked = (app.Forms(cbfName).TuxValue() = 7) : Check "run code behind form", checked
checked = Empty : checked = (app.Forms(cbfName).Tag = "onopen passed") : Check "form open event", checked
app.DoCmd.Close 2, cbfName
app.DoCmd.DeleteObject 2, cbfName
checked = Empty : checked = (Err.Number = 0 And app.CurrentProject.AllForms.Count = 1) : Check "delete form with code", checked
Err.Clear

' A report, previewed and exported
Dim rpt, rptName, rctl
Set rpt = app.CreateReport()
checked = Empty : checked = (Not rpt Is Nothing) : Check "CreateReport", checked
rpt.RecordSource = "Items"
Set rctl = app.CreateReportControl(rpt.Name, 109, 0, "", "Name", 500, 100, 3000, 400)
rptName = rpt.Name
app.DoCmd.Save 3, rptName
app.DoCmd.Close 3, rptName, 1
checked = Empty : checked = (Err.Number = 0) : Check "save report", checked
app.DoCmd.OpenReport rptName, 2
checked = Empty : checked = (app.Reports(rptName).Name = rptName) : Check "report preview", checked
WScript.Sleep 800
app.DoCmd.Close 3, rptName
Err.Clear
Dim pdf : pdf = dir & "\access-report.pdf"
Remove pdf
app.DoCmd.OutputTo 3, rptName, "PDF Format (*.pdf)", pdf
checked = Empty : checked = (SizeOf(pdf) > 1000) : Check "report to PDF", checked

' VBA module, compiled and run
Dim vbProj : Set vbProj = app.VBE.ActiveVBProject
checked = Empty : checked = (Not vbProj Is Nothing) : Check "VBE project", checked
Dim comp : Set comp = vbProj.VBComponents.Add(1)
comp.CodeModule.AddFromString "Public Function TuxAdd(a, b)" & vbLf & "TuxAdd = a + b" & vbLf & "End Function"
checked = Empty : checked = (comp.CodeModule.CountOfLines >= 3) : Check "add VBA module", checked
checked = Empty : checked = (app.Run("TuxAdd", 40, 2) = 42) : Check "run VBA", checked
' ADO on Access's own connection, from VBA as a real database would use it
Dim adoComp : Set adoComp = vbProj.VBComponents.Add(1)
adoComp.CodeModule.AddFromString Join(Array( _
 "Public Function TuxAdo() As String", _
 "Dim c, r, s", _
 "Set c = CurrentProject.Connection", _
 "Set r = CreateObject(""ADODB.Recordset"")", _
 "r.Open ""SELECT Id, Name FROM Items WHERE Id <= 2 ORDER BY Id"", c, 3, 1", _
 "s = r.RecordCount & "":""", _
 "Do While Not r.EOF : s = s & r.Fields(1).Value & "";"" : r.MoveNext : Loop", _
 "r.Close", _
 "r.Open ""Items"", c, 1, 3, 2", _
 "r.AddNew : r.Fields(""Name"").Value = ""FromAdo"" : r.Update", _
 "r.Close", _
 "TuxAdo = s & DCount(""*"", ""Items"", ""Name='FromAdo'"")", _
 "End Function"), vbLf)
Dim adoResult : adoResult = app.Run("TuxAdo")
WScript.Echo "INFO ADO in VBA returned " & adoResult
checked = Empty : checked = (adoResult = "2:Item 1;Item 2;1") : Check "ADO recordsets in VBA", checked
app.DoCmd.Save 5, adoComp.Name
Set adoComp = Nothing
Err.Clear
' Saved, or closing the database asks for the module's name
app.DoCmd.Save 5, comp.Name
Err.Clear

' Exports
Dim xlsx, txt
xlsx = dir & "\access-export.xlsx" : txt = dir & "\access-export.txt"
Remove xlsx : Remove txt
app.DoCmd.TransferSpreadsheet 1, 10, "Items", xlsx, True
checked = Empty : checked = (SizeOf(xlsx) > 1000) : Check "export xlsx", checked
app.DoCmd.TransferText 2, , "Items", txt, True
checked = Empty : checked = (SizeOf(txt) > 1000) : Check "export text", checked
Dim xml : xml = dir & "\access-export.xml"
Remove xml
app.ExportXML 0, "Items", xml
checked = Empty : checked = (SizeOf(xml) > 1000) : Check "export xml", checked

' Import back into a new table through the Excel engine
app.DoCmd.TransferSpreadsheet 0, 10, "Imported", xlsx, True
checked = Empty : checked = (Scalar("SELECT COUNT(*) FROM Imported") = Scalar("SELECT COUNT(*) FROM Items")) : Check "import xlsx", checked

' ADO through Access's own connection
Dim conn : Set conn = app.CurrentProject.Connection
checked = Empty : checked = (conn.State = 1) : Check "ADO connection", checked
checked = Empty : checked = (AdoCount(conn) >= 1500) : Check "ADO query", checked
Set conn = Nothing

' Any object still held from this database keeps its file open
Set db = Nothing : Set rs = Nothing : Set comp = Nothing : Set vbProj = Nothing
Set frm = Nothing : Set ctl = Nothing : Set cbf = Nothing : Set rpt = Nothing : Set rctl = Nothing
app.CloseCurrentDatabase
checked = Empty : checked = (Err.Number = 0) : Check "close database", checked
Dim compacted : compacted = dir & "\access-compact.accdb"
Remove compacted
app.DBEngine.CompactDatabase dbPath, compacted
checked = Empty : checked = (SizeOf(compacted) > 1000) : Check "compact", checked
app.OpenCurrentDatabase dbPath
Set db = app.CurrentDb
checked = Empty : checked = (Scalar("SELECT COUNT(*) FROM Items") >= 1500) : Check "reopen", checked
checked = Empty : checked = (app.CurrentProject.AllForms.Count = 1) : Check "reopen form exists", checked
app.CloseCurrentDatabase
app.Quit
checked = Empty : checked = (Err.Number = 0) : Check "quit", checked
WScript.Echo "RESULT access passed=" & passes & " failed=" & fails
