# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read a chart workbook saved directly or inside an OLE Package stream."""
import io
from pathlib import Path
import subprocess
import tempfile
from zipfile import ZipFile


def chart_workbook(presentation):
    entries = [name for name in presentation.namelist() if name.startswith("ppt/embeddings/")]
    assert len(entries) == 1, "Expected one embedded chart workbook"
    data = presentation.read(entries[0])
    if data.startswith(bytes.fromhex("d0cf11e0a1b11ae1")):
        with tempfile.TemporaryDirectory(prefix="tuxoffice-chart-") as directory:
            source = Path(directory)/"chart.bin"
            source.write_bytes(data)
            data = subprocess.check_output(["7z","x","-so",str(source),"Package"],
                                           stderr=subprocess.DEVNULL,timeout=10)
    return ZipFile(io.BytesIO(data))
