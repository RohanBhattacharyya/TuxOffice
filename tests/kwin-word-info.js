// SPDX-License-Identifier: LGPL-2.1-or-later
for (const w of workspace.windowList()) {
    if (w.resourceClass === "winword.exe" && w.normalWindow) {
        print("TUXCAPTURE", w.internalId);
        break;
    }
}
