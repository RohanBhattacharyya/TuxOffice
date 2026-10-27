// SPDX-License-Identifier: LGPL-2.1-or-later
for (const w of workspace.windowList()) {
    if (w.resourceClass === "winword.exe" && w.normalWindow) {
        w.minimized = false;
        w.setMaximize(true, true);
        workspace.raiseWindow(w);
        workspace.activeWindow = w;
        print("TUXWORD", w.internalId, w.frameGeometry.x, w.frameGeometry.y,
              w.frameGeometry.width, w.frameGeometry.height);
        break;
    }
}
