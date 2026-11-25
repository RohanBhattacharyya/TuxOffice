#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the small Windows helpers into the selected Office test prefix.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tux_home="${TUXOFFICE_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice}"
prefix="${TUXOFFICE_PREFIX:-$tux_home/prefixes/office}"
destination="$prefix/drive_c/tuxoffice"
[[ -f $prefix/system.reg ]] || { echo "Office prefix missing: $prefix" >&2; exit 1; }
mkdir -p "$destination"
winegcc -b x86_64-windows -mconsole -o "$destination/typer.exe" "$HERE/typer.c" -luser32 -loleacc -lole32 -loleaut32 -luuid
winegcc -b x86_64-windows -mconsole -municode -o "$destination/winctl.exe" "$HERE/winctl.c" -luser32 -loleacc -lole32 -loleaut32 -luuid
winegcc -b x86_64-windows -mconsole -municode -o "$destination/onenote-api.exe" "$HERE/onenote-api.c" -lole32 -loleaut32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/office-probe.exe" "$HERE/office-probe.c" \
  -loleaut32 -lole32 -loleacc -luser32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/office-functional.exe" "$HERE/office-functional.c" \
  -loleaut32 -lole32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/caption-buttons.exe" "$HERE/caption-buttons.c" -luser32 -lgdi32
winegcc -b x86_64-windows -mconsole -o "$destination/msgbox-close.exe" "$HERE/msgbox-close.c" -luser32
winegcc -b x86_64-windows -mconsole -municode -o "$destination/file-dialog-probe.exe" "$HERE/file-dialog-probe.c" -lole32 -lshell32 -luuid
winegcc -b i386-windows -mconsole -municode -o "$destination/file-dialog-probe32.exe" "$HERE/file-dialog-probe.c" -lole32 -lshell32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/native-dialog-focus.exe" "$HERE/native-dialog-focus.c" -lole32 -luuid -luser32
winegcc -b i386-windows -mconsole -o "$destination/native-dialog-focus32.exe" "$HERE/native-dialog-focus.c" -lole32 -luuid -luser32
winegcc -b x86_64-windows -mconsole -o "$destination/ado-lifetime.exe" "$HERE/ado-lifetime.c" -lole32 -loleaut32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/native-path.exe" "$HERE/native-path.c" -lntdll
winegcc -b i386-windows -mconsole -o "$destination/native-path32.exe" "$HERE/native-path.c" -lntdll
winegcc -b x86_64-windows -mconsole -municode -o "$destination/typelib-load.exe" "$HERE/typelib-load.c" -loleaut32 -lole32
winegcc -b i386-windows -mconsole -municode -o "$destination/typelib-load32.exe" "$HERE/typelib-load.c" -loleaut32 -lole32
winegcc -b x86_64-windows -mconsole -o "$destination/typelib-dual.exe" "$HERE/typelib-dual.c" -loleaut32 -lole32 -luuid -luser32
winegcc -b i386-windows -mconsole -o "$destination/typelib-dual32.exe" "$HERE/typelib-dual.c" -loleaut32 -lole32 -luuid -luser32
winegcc -b x86_64-windows -mconsole -o "$destination/ado-interfaces.exe" "$HERE/ado-interfaces.c" -loleaut32 -lole32 -luuid -luser32
winegcc -b i386-windows -mconsole -o "$destination/ado-interfaces32.exe" "$HERE/ado-interfaces.c" -loleaut32 -lole32 -luuid -luser32
winegcc -b x86_64-windows -mconsole -o "$destination/ado-stream.exe" "$HERE/ado-stream.c" -loleaut32 -lole32 -luuid
winegcc -b i386-windows -mconsole -o "$destination/ado-stream32.exe" "$HERE/ado-stream.c" -loleaut32 -lole32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/typelib-index.exe" "$HERE/typelib-index.c" -loleaut32 -lole32 -luuid
winegcc -b i386-windows -mconsole -o "$destination/typelib-index32.exe" "$HERE/typelib-index.c" -loleaut32 -lole32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/ado-layout.exe" "$HERE/ado-layout.c" -loleaut32 -lole32 -luuid
winegcc -b i386-windows -mconsole -o "$destination/ado-layout32.exe" "$HERE/ado-layout.c" -loleaut32 -lole32 -luuid
