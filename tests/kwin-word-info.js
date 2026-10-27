for (const w of workspace.windowList()) {
    if (w.resourceClass === "winword.exe" && w.normalWindow) {
        print("TUXCAPTURE", w.internalId);
        break;
    }
}
