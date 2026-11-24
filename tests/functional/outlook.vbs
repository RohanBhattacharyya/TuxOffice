' SPDX-License-Identifier: LGPL-2.1-or-later
' Outlook object-model test on a local PST profile: never touches a server.
' Usage: cscript //nologo outlook.vbs C:\work\dir
Option Explicit
Dim checked
Dim dir, app, ns, fails, passes
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

On Error Resume Next
Set app = CreateObject("Outlook.Application")
checked = Empty : checked = (Not app Is Nothing) : Check "create Outlook.Application", checked
Set ns = app.GetNamespace("MAPI")
checked = Empty : checked = (Not ns Is Nothing) : Check "MAPI namespace", checked
ns.Logon "TuxTest", "", False, True
checked = Empty : checked = (Err.Number = 0) : Check "logon", checked
checked = Empty : checked = (Left(app.Version, 2) = "16") : Check "version", checked
Dim inbox, cal, contacts, tasks, notes, drafts
Set inbox = ns.GetDefaultFolder(6)
checked = Empty : checked = (Not inbox Is Nothing) : Check "inbox", checked
Set cal = ns.GetDefaultFolder(9)
checked = Empty : checked = (Not cal Is Nothing) : Check "calendar", checked
Set contacts = ns.GetDefaultFolder(10)
checked = Empty : checked = (Not contacts Is Nothing) : Check "contacts", checked
Set tasks = ns.GetDefaultFolder(13)
checked = Empty : checked = (Not tasks Is Nothing) : Check "tasks", checked
Set drafts = ns.GetDefaultFolder(16)
checked = Empty : checked = (Not drafts Is Nothing) : Check "drafts", checked

' Outlook exits when its last window closes, so keep an explorer open.
inbox.Display
checked = Empty : checked = (app.Explorers.Count >= 1) : Check "explorer window", checked
WScript.Sleep 2000

Dim mail, i, before
before = drafts.Items.Count
Set mail = app.CreateItem(0)
mail.Subject = "TuxOffice test message"
mail.To = "nobody@example.invalid"
mail.HTMLBody = "<html><body><b>Bold</b> body</body></html>"
mail.Save
checked = Empty : checked = (drafts.Items.Count = before + 1) : Check "save draft", checked
Dim found : Set found = drafts.Items.Find("[Subject] = 'TuxOffice test message'")
checked = Empty : checked = (Not found Is Nothing) : Check "find draft", checked
checked = Empty : checked = (InStr(found.HTMLBody, "Bold") > 0) : Check "html body kept", checked
mail.Display
checked = Empty : checked = (app.Inspectors.Count >= 1) : Check "compose window", checked
WScript.Sleep 2000
mail.Close 1
Err.Clear

Dim started : started = Timer
For i = 1 To 200
  Set mail = app.CreateItem(0)
  mail.Subject = "Bulk message " & i
  mail.Body = "Body of message " & i
  mail.Move inbox
Next
WScript.Echo "TIME create200 " & FormatNumber(Timer - started, 3)
checked = Empty : checked = (inbox.Items.Count >= 200) : Check "bulk mail", checked

Dim appt : Set appt = app.CreateItem(1)
appt.Subject = "TuxOffice appointment"
appt.Start = DateAdd("d", 1, Date) + TimeSerial(10, 0, 0)
appt.Duration = 45
appt.Location = "Room 1"
appt.Save
checked = Empty : checked = (cal.Items.Find("[Subject] = 'TuxOffice appointment'").Duration = 45) : Check "appointment", checked

Dim c : Set c = app.CreateItem(2)
c.FullName = "Tux Penguin"
c.Email1Address = "tux@example.invalid"
c.Save
checked = Empty : checked = (contacts.Items.Find("[FullName] = 'Tux Penguin'").Email1Address = "tux@example.invalid") : Check "contact", checked

Dim t : Set t = app.CreateItem(3)
t.Subject = "TuxOffice task"
t.Save
checked = Empty : checked = (tasks.Items.Find("[Subject] = 'TuxOffice task'").Subject = "TuxOffice task") : Check "task", checked

' Deleting a folder moves it to Deleted Items as "TuxTest (n)"; after about twenty runs
' Outlook moves no more there, the inbox keeps its TuxTest and adding it fails. Purge
' leftovers from both, deleting from Deleted Items for good.
Dim deleted : Set deleted = ns.GetDefaultFolder(3)
For i = deleted.Folders.Count To 1 Step -1
  If Left(deleted.Folders(i).Name, 7) = "TuxTest" Then deleted.Folders(i).Delete
Next
For i = inbox.Folders.Count To 1 Step -1
  If Left(inbox.Folders(i).Name, 7) = "TuxTest" Then inbox.Folders(i).Delete
Next
For i = deleted.Folders.Count To 1 Step -1
  If Left(deleted.Folders(i).Name, 7) = "TuxTest" Then deleted.Folders(i).Delete
Next
Err.Clear
Dim sub1 : Set sub1 = inbox.Folders.Add("TuxTest")
checked = Empty : checked = (inbox.Folders("TuxTest").Name = "TuxTest") : Check "add folder", checked
sub1.Delete
checked = Empty : checked = (Err.Number = 0) : Check "delete folder", checked
For i = deleted.Folders.Count To 1 Step -1
  If Left(deleted.Folders(i).Name, 7) = "TuxTest" Then deleted.Folders(i).Delete
Next
Err.Clear

Set mail = inbox.Items.Find("[Subject] = 'Bulk message 7'")
mail.UnRead = False
mail.FlagRequest = "Follow up"
mail.Save
checked = Empty : checked = (inbox.Items.Find("[Subject] = 'Bulk message 7'").FlagRequest = "Follow up") : Check "flag mail", checked
mail.SaveAs dir & "\outlook-mail.msg", 3
checked = Empty : checked = (SizeOf(dir & "\outlook-mail.msg") > 1000) : Check "save msg", checked
Dim loaded : Set loaded = ns.OpenSharedItem(dir & "\outlook-mail.msg")
checked = Empty : checked = (loaded.Subject = "Bulk message 7") : Check "open msg", checked
loaded.Close 1
Err.Clear
mail.SaveAs dir & "\outlook-mail.mht", 10
checked = Empty : checked = (SizeOf(dir & "\outlook-mail.mht") > 500) : Check "save mht", checked
Err.Clear

Dim att : Set att = app.CreateItem(0)
att.Subject = "Attachment test"
att.Attachments.Add dir & "\outlook-mail.msg"
checked = Empty : checked = (att.Attachments.Count = 1) : Check "attachment", checked
att.Close 1
Err.Clear

Set app.ActiveExplorer.CurrentFolder = cal
checked = Empty : checked = (Err.Number = 0) : Check "calendar view", checked
WScript.Sleep 2000

started = Timer
Dim n : n = 0
Dim items : Set items = inbox.Items
For i = 1 To items.Count
  If InStr(items.Item(i).Subject, "Bulk") = 1 Then n = n + 1
Next
WScript.Echo "TIME iterate " & FormatNumber(Timer - started, 3)
checked = Empty : checked = (n >= 200) : Check "iterate", checked

ns.Logoff
app.Quit
checked = Empty : checked = (Err.Number = 0) : Check "quit", checked
WScript.Echo "RESULT outlook passed=" & passes & " failed=" & fails
