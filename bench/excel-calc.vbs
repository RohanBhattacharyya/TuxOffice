' SPDX-License-Identifier: LGPL-2.1-or-later
' Excel compute benchmark, driven over COM automation.
' Run inside the prefix:  tuxoffice wine cscript //nologo bench\excel-calc.vbs
' Run the same file on Windows (VM or bare metal) to compare.
Option Explicit
Dim t0, xl, wb, ws, i, r

t0 = Timer
Set xl = CreateObject("Excel.Application")
WScript.Echo "excel_start_s=" & FormatNumber(Timer - t0, 2)

xl.ScreenUpdating = False
Set wb = xl.Workbooks.Add
Set ws = wb.Worksheets(1)

' 200k rows x 3 dependent formula columns = 600k formulas.
t0 = Timer
ws.Range("A1:A200000").Formula = "=ROW()*1.5"
ws.Range("B1:B200000").Formula = "=SQRT(A1)+SIN(A1)"
ws.Range("C1:C200000").Formula = "=IF(MOD(ROW(),2)=0,B1*2,B1/2)"
ws.Range("D1").Formula = "=SUMPRODUCT(B1:B200000,C1:C200000)"
WScript.Echo "fill_s=" & FormatNumber(Timer - t0, 2)

t0 = Timer
For i = 1 To 5
  xl.CalculateFull
Next
WScript.Echo "calcfull_x5_s=" & FormatNumber(Timer - t0, 2)

t0 = Timer
r = 0
Dim v: v = ws.Range("C1:C200000").Value
For i = 1 To 200000
  r = r + v(i, 1)
Next
WScript.Echo "readback_s=" & FormatNumber(Timer - t0, 2) & " checksum=" & FormatNumber(r, 2) & " d1=" & ws.Range("D1").Value

wb.Close False
xl.Quit
