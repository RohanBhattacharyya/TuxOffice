// SPDX-License-Identifier: LGPL-2.1-or-later
// Plain white desktop stand-in for the isolated KWin session, so Office's
// frame shadows are visible in screen captures. Run with qml6 inside
// tests/isolated-run.py; KWin keeps it below other windows.
import QtQuick
import QtQuick.Window

Window {
    title: "TuxOffice backdrop"
    visibility: Window.FullScreen
    flags: Qt.WindowStaysOnBottomHint | Qt.FramelessWindowHint
    color: "white"
    visible: true
}
