' SPDX-License-Identifier: LGPL-2.1-or-later
' Compile and execute early-bound ADO 6.1 code in the native Access VBA host.
' Usage: cscript //nologo ado.vbs C:\work\dir
Option Explicit
Dim app, project, module, code, result, path, fs, passes, fails
passes = 0 : fails = 0
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

Set fs = CreateObject("Scripting.FileSystemObject")
path = WScript.Arguments(0) & "\ado-vba.accdb"
If fs.FileExists(path) Then fs.DeleteFile path
Set app = CreateObject("Access.Application")
Check "create Access", Not app Is Nothing
app.Visible = True
app.NewCurrentDatabase path
Check "create database", fs.FileExists(path)
Set project = app.VBE.ActiveVBProject
project.References.AddFromGuid "{B691E011-1797-432E-907A-4D8C69339129}", 6, 1
Check "reference ADO 6.1", True
Set module = project.VBComponents.Add(1)
module.Name = "TuxAdo61"
code = Join(Array( _
  "Public Function ExerciseAdo61() As String", _
  "Dim cn As ADODB.Connection, rs As ADODB.Recordset, field As ADODB.Field", _
  "Dim cmd As ADODB.Command, parameter As ADODB.Parameter, stream As ADODB.Stream", _
  "Set cn = New ADODB.Connection", _
  "ExerciseAdo61 = CStr(cn.State)", _
  "Set rs = New ADODB.Recordset", _
  "rs.Fields.Append ""Quantity"", adInteger", _
  "rs.Open", _
  "rs.AddNew", _
  "Set field = rs.Fields(0)", _
  "field.Value = 42", _
  "rs.Update", _
  "ExerciseAdo61 = ExerciseAdo61 & ""|"" & rs.RecordCount & ""|"" & field.Value", _
  "rs.Close", _
  "Set cmd = New ADODB.Command", _
  "Set parameter = cmd.CreateParameter(""Quantity"", adInteger, adParamInput, 4, 42)", _
  "cmd.Parameters.Append parameter", _
  "ExerciseAdo61 = ExerciseAdo61 & ""|"" & cmd.Parameters.Count", _
  "Set stream = New ADODB.Stream", _
  "stream.Open", _
  "stream.WriteText ""ADO 6.1 text""", _
  "stream.Position = 0", _
  "ExerciseAdo61 = ExerciseAdo61 & ""|"" & stream.ReadText", _
  "stream.Close", _
  "End Function"), vbCrLf)
module.CodeModule.AddFromString code
Check "insert typed ADO macro", module.CodeModule.CountOfLines > 20
app.DoCmd.Save 5, module.Name
Check "save typed ADO macro", True
result = app.Run("ExerciseAdo61")
Check "early-bound recordset, command and stream (length " & Len(result) & ")", _
  result = "0|1|42|1|ADO 6.1 text" And Len(result) = Len("0|1|42|1|ADO 6.1 text")
app.CloseCurrentDatabase
app.Quit 2
Check "close Access", True
WScript.Echo "RESULT ado passed=" & passes & " failed=" & fails
