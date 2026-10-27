// SPDX-License-Identifier: LGPL-2.1-or-later
// Bring the first Word window to the front and report its frame geometry.
for (const w of workspace.windowList()) {
    if (w.resourceClass === "winword.exe" && w.normalWindow) {
        w.minimized = false;
        workspace.raiseWindow(w);
        workspace.activeWindow = w;
        const g = w.frameGeometry;
        print("TUXWORD", g.x, g.y, g.width, g.height, workspace.activeWindow === w ? "active" : "not-active");
        break;
    }
}
