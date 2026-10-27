#!/usr/bin/env bash
# Start an isolated KWin virtual display and a reflinked test prefix.
# Usage: isolated-session.sh OUTDIR
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ${1:-} != --inside ]]; then
  out=$(realpath -m "${1:?output directory}")
  live_home="${TUXOFFICE_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/tuxoffice}"
  live_prefix="${TUXOFFICE_PREFIX:-$live_home/prefixes/office}"
  mkdir -p "$out/home" "$out/config"
  [[ -d $out/home/prefix ]] || cp -a --reflink=auto "$live_prefix" "$out/home/prefix"
  ln -sfn "$live_home/runners" "$out/home/runners"
  ln -sfn "$live_home/tools" "$out/home/tools"
  mkdir -p "$out/home/cache"
  TUXOFFICE_PREFIX="$out/home/prefix" "$HERE/build-helpers.sh"
  # The session bus must not know the live desktop's display: services it
  # starts, such as the file chooser portal, would open their windows there.
  exec env -u DISPLAY -u WAYLAND_DISPLAY dbus-run-session -- "$0" --inside "$out"
fi
out=$2
export WAYLAND_DISPLAY="tuxoffice-test-$$"
dbus-update-activation-environment WAYLAND_DISPLAY="$WAYLAND_DISPLAY" QT_QPA_PLATFORM=wayland \
  XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland
export TUXOFFICE_HOME="$out/home" TUXOFFICE_PREFIX="$out/home/prefix"
export TUXOFFICE_SCALE=1.25 XDG_CONFIG_HOME="$out/config"
export QT_LOGGING_TO_CONSOLE=1
if [[ -n ${TUXOFFICE_TEST_VK_DRIVER_FILES:-} ]]; then
  export VK_DRIVER_FILES="$TUXOFFICE_TEST_VK_DRIVER_FILES"
elif [[ -r /usr/share/vulkan/icd.d/intel_icd.json ]] && [[ -r /usr/share/vulkan/icd.d/nvidia_icd.json ]]; then
  # NVIDIA's Wayland WSI crashes against the virtual framebuffer on this
  # hybrid system. Exercise Wine's Vulkan path on the display's Intel GPU.
  export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.json
  # Composite on the same GPU, as the live session does. NVIDIA's EGL does not
  # wait for Intel's implicit fences, so captures showed half-written frames.
  kwin_egl=/usr/share/glvnd/egl_vendor.d/50_mesa.json
fi
export TUX_KWIN_LOG="$out/kwin.log"
kwin_egl=${kwin_egl:-}
unset DISPLAY
# Sizes are logical: 2048x1280 at 1.25 is the 2560x1600 panel of the live desktop.
${kwin_egl:+env __EGL_VENDOR_LIBRARY_FILENAMES=$kwin_egl} kwin_wayland --virtual --socket "$WAYLAND_DISPLAY" --width "${TUXOFFICE_TEST_WIDTH:-2048}" \
  --height "${TUXOFFICE_TEST_HEIGHT:-1280}" --scale 1.25 \
  --no-lockscreen --no-global-shortcuts --no-kactivities >"$TUX_KWIN_LOG" 2>&1 &
compositor=$!
export TUX_KWIN_PID="$compositor"
cleanup() { "$HERE/../tuxoffice" kill; kill "$compositor" 2>/dev/null || true; }
trap cleanup EXIT
for _ in $(seq 1 100); do
  kill -0 "$compositor" || { cat "$TUX_KWIN_LOG"; exit 1; }
  if [[ -S $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY ]] && qdbus6 org.kde.KWin /KWin >/dev/null 2>&1; then break; fi
  sleep 0.2
done
# --virtual multiplies the mode by --scale but leaves the output at scale 1.
# Apply the fractional scale through output management, as on the live panel.
KSCREEN_BACKEND_INPROCESS=1 kscreen-doctor output.Virtual-0.scale.1.25 >/dev/null 2>&1 ||
  echo "warning: could not set the virtual output scale" >&2
python3 - "$out/environment.json" <<'PY'
import json, os, sys
keys = ('DBUS_SESSION_BUS_ADDRESS', 'WAYLAND_DISPLAY', 'TUXOFFICE_HOME', 'TUXOFFICE_PREFIX',
        'TUXOFFICE_SCALE', 'XDG_CONFIG_HOME', 'TUX_KWIN_LOG', 'TUX_KWIN_PID')
with open(sys.argv[1], 'w') as f:
    values = {key: os.environ[key] for key in keys}
    if 'VK_DRIVER_FILES' in os.environ:
        values['VK_DRIVER_FILES'] = os.environ['VK_DRIVER_FILES']
    json.dump(values, f)
PY
echo "isolated Wayland session ready: $out"
wait "$compositor"
