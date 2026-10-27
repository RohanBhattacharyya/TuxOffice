#!/usr/bin/env bash
# Build the small Windows helpers into the selected Office test prefix.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tux_home="${TUXOFFICE_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice}"
prefix="${TUXOFFICE_PREFIX:-$tux_home/prefixes/office}"
destination="$prefix/drive_c/tuxoffice"
[[ -f $prefix/system.reg ]] || { echo "Office prefix missing: $prefix" >&2; exit 1; }
mkdir -p "$destination"
winegcc -b x86_64-windows -mconsole -o "$destination/typer.exe" "$HERE/typer.c" -luser32 -loleacc -lole32 -loleaut32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/office-probe.exe" "$HERE/office-probe.c" \
  -loleaut32 -lole32 -loleacc -luser32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/office-functional.exe" "$HERE/office-functional.c" \
  -loleaut32 -lole32 -luuid
winegcc -b x86_64-windows -mconsole -o "$destination/caption-buttons.exe" "$HERE/caption-buttons.c" -luser32 -lgdi32
winegcc -b x86_64-windows -mconsole -o "$destination/msgbox-close.exe" "$HERE/msgbox-close.c" -luser32
winegcc -b x86_64-windows -mconsole -municode -o "$destination/file-dialog-probe.exe" "$HERE/file-dialog-probe.c" -lole32 -lshell32 -luuid
