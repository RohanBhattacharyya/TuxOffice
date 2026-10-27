' SPDX-License-Identifier: LGPL-2.1-or-later
' Open a document in an invisible Word and force layout so Office fetches
' every cloud font it references, then quit once the font cache settles.
' Usage: cscript //nologo cloudfonts.vbs C:\path\fonts.docx [max-seconds]
Option Explicit
Dim wd, doc, maxSecs, t0, fso, cache, last, stableSince, n

maxSecs = 120
If WScript.Arguments.Count > 1 Then maxSecs = CInt(WScript.Arguments(1))

Set fso = CreateObject("Scripting.FileSystemObject")
cache = CreateObject("WScript.Shell").ExpandEnvironmentStrings("%LOCALAPPDATA%") & _
        "\Microsoft\FontCache\4\CloudFonts"

Function CountFonts(path)
  Dim folder, child, total
  total = 0
  If fso.FolderExists(path) Then
    Set folder = fso.GetFolder(path)
    total = folder.Files.Count
    For Each child In folder.SubFolders
      total = total + CountFonts(child.Path)
    Next
  End If
  CountFonts = total
End Function

Set wd = CreateObject("Word.Application")
wd.Visible = False
wd.DisplayAlerts = 0
Set doc = wd.Documents.Open(WScript.Arguments(0), False, True)

' Downloads land asynchronously; re-layout until the cache stops growing.
t0 = Timer
last = -1
stableSince = Timer
Do While Timer - t0 < maxSecs
  doc.Repaginate
  WScript.Sleep 2000
  n = CountFonts(cache)
  If n <> last Then
    last = n
    stableSince = Timer
  ElseIf Timer - stableSince > 10 And Timer - t0 > 15 Then
    Exit Do
  End If
Loop
WScript.Echo "cloud fonts cached: " & last

doc.Close False
wd.Quit
