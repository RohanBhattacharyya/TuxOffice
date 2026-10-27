' SPDX-License-Identifier: LGPL-2.1-or-later
' Drive visible activity in the running Word for rendering tests:
' typing, new paragraphs, scrolling and a selection, at roughly human pace.
' Usage: cscript //nologo word-activity.vbs [seconds]
Option Explicit
Dim wd, sel, secs, t0, i

secs = 12
If WScript.Arguments.Count > 0 Then secs = CInt(WScript.Arguments(0))

Set wd = GetObject(, "Word.Application")
Set sel = wd.Selection
sel.EndKey 6  ' wdStory

t0 = Timer
i = 0
Do While Timer - t0 < secs
  sel.TypeText "flicker test " & i & " "
  If i Mod 6 = 5 Then sel.TypeParagraph
  If i Mod 10 = 9 Then
    wd.ActiveWindow.SmallScroll 3
    wd.ActiveWindow.SmallScroll , 3
  End If
  i = i + 1
  WScript.Sleep 150
Loop
sel.HomeKey 5, 1  ' select to start of line
WScript.Echo "typed " & i & " chunks"
