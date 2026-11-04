' SPDX-License-Identifier: LGPL-2.1-or-later
' PowerPoint video: export a deck as MP4 through Media Foundation's sink writer
' (converter, H.264 encoder, MP4 sink), then insert that video into a slide,
' which reads it back through a source reader. Playing it in a slide show is
' not tried: PowerPoint asks the media engine for a service Wine lacks and
' stops itself (see docs/STATUS.md).
' Usage: cscript //nologo media.vbs C:\work\dir
Option Explicit
Dim dir, fails, passes, fso, pp, pres, sl, i, t0, status, m, slides
dir = WScript.Arguments(0) & "\media"
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

Set pp = CreateObject("PowerPoint.Application")
pp.Visible = True
Set pres = pp.Presentations.Add(True)
For i = 1 To 3
  Set sl = pres.Slides.Add(i, 2)
  sl.Shapes(1).TextFrame.TextRange.Text = "TuxOffice video slide " & i
  sl.Shapes(2).TextFrame.TextRange.Text = "Exported through Media Foundation"
  sl.SlideShowTransition.AdvanceOnTime = True
  sl.SlideShowTransition.AdvanceTime = 1
Next
Check "deck", pres.Slides.Count = 3

pres.CreateVideo dir & "\deck.mp4", False, 1, 480, 24, 60
Check "video export starts", True
t0 = Timer
Do
  status = pres.CreateVideoStatus
  If status <> 1 And status <> 2 Then Exit Do
  WScript.Sleep 500
Loop While Timer - t0 < 180
Check "video export finishes (status " & status & ")", status = 3
Check "video file written", fso.FileExists(dir & "\deck.mp4")
If fso.FileExists(dir & "\deck.mp4") Then Check "video has content", fso.GetFile(dir & "\deck.mp4").Size > 10000

Set sl = pres.Slides.Add(4, 12)
Set m = sl.Shapes.AddMediaObject2(dir & "\deck.mp4", False, True, 50, 50, 426, 240)
Check "video inserted", m.Type = 16
Check "inserted video is about 3 s long (" & m.MediaFormat.Length & " ms)", Abs(m.MediaFormat.Length - 3000) < 500
sl.Export dir & "\video-slide.png", "PNG", 960, 540
Check "slide with video exports", fso.FileExists(dir & "\video-slide.png")
pres.SaveAs dir & "\with-video.pptx"
Check "deck with video saves", fso.FileExists(dir & "\with-video.pptx")
pres.Close
pp.Quit
Check "powerpoint quits", True
WScript.Echo "RESULT media passed=" & passes & " failed=" & fails
