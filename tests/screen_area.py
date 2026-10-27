"""KWin pixel capture shared by the Office latency measurements."""
import os
import threading

import dbus
from PIL import Image


class ScreenArea:
    def __init__(self, x, y, width, height):
        self.x, self.y, self.width, self.height = x, y, width, height
        self.service = dbus.Interface(dbus.SessionBus().get_object(
            "org.kde.KWin", "/org/kde/KWin/ScreenShot2"), "org.kde.KWin.ScreenShot2")
        self.options = dbus.Dictionary({"include-cursor":False}, signature="sv")

    def capture(self):
        return self._capture(self.service.CaptureArea,self.x,self.y,dbus.UInt32(self.width),dbus.UInt32(self.height))

    def _capture(self, method, *args):
        read_fd, write_fd = os.pipe()
        chunks = []
        def drain():
            with os.fdopen(read_fd, "rb") as stream:
                while data := stream.read(262144):
                    chunks.append(data)
        thread = threading.Thread(target=drain, daemon=True)
        thread.start()
        try:
            info = method(*args, self.options, dbus.types.UnixFd(write_fd), timeout=5)
        finally:
            os.close(write_fd)
        thread.join(5)
        if thread.is_alive():
            raise RuntimeError("Capture pipe timed out")
        return info, b"".join(chunks)

    @staticmethod
    def image(info, data):
        formats = {4:"BGRX",5:"BGRA",6:"BGRA",16:"RGBX",17:"RGBA",18:"RGBA"}
        mode = "RGB" if int(info["format"]) in (4,16) else "RGBA"
        return Image.frombytes(mode, (int(info["width"]),int(info["height"])), data,
            "raw", formats[int(info["format"])], int(info["stride"]))


class WindowCapture(ScreenArea):
    """Capture a particular live window without depending on desktop focus."""
    def __init__(self, handle):
        super().__init__(0,0,1,1)
        self.handle=handle
        self.options['include-decoration']=False

    def capture(self):
        return self._capture(self.service.CaptureWindow,self.handle)
