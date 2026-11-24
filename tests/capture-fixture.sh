#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# A private PipeWire for capture tests: no hardware, a microphone playing a 440 Hz
# tone (TestMic) and a test pattern camera (TuxTestCam). Nothing touches the real
# audio session or opens a real device: WirePlumber's hardware monitors are off.
#
# usage: tests/capture-fixture.sh start DIR   prints an env file to source
#        tests/capture-fixture.sh stop DIR
# then:  . DIR/env.sh; WINEPREFIX=... wine mf-capture.exe
# The runtime directory is short (/run/user/UID/tuxoffice-test-pw): socket paths
# are limited to 108 bytes.
set -euo pipefail
cmd=${1:?start|stop}; dir=$(realpath -m "${2:?state directory}")
run=/run/user/$(id -u)/tuxoffice-test-pw

if [[ $cmd == stop ]]; then
  [[ -f $dir/pids ]] && while read -r pid; do kill "$pid" 2>/dev/null || true; done <"$dir/pids"
  rm -f "$dir/pids"
  exit 0
fi

mkdir -p "$dir/config/wireplumber/wireplumber.conf.d" "$dir/state" "$run"
chmod 700 "$run"
cat >"$dir/config/wireplumber/wireplumber.conf.d/90-no-hardware.conf" <<'EOF'
wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
  }
}
EOF
cat >"$dir/env.sh" <<EOF
export XDG_RUNTIME_DIR=$run PIPEWIRE_RUNTIME_DIR=$run XDG_CONFIG_HOME=$dir/config XDG_STATE_HOME=$dir/state
export PULSE_SERVER=unix:$run/pulse/native PULSE_RUNTIME_PATH=$run/pulse
unset DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY DISPLAY
EOF
python3 - "$dir/tone.wav" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<hh', v, v) for v in
              (int(0.5 * 32767 * math.sin(2 * math.pi * 440 * i / 48000)) for i in range(48000 * 30))))
w.close()
EOF

# shellcheck source=/dev/null
. "$dir/env.sh"
: >"$dir/pids"
start() { setsid "$@" >>"$dir/fixture.log" 2>&1 & echo $! >>"$dir/pids"; }
start pipewire; sleep 1
start wireplumber
start pipewire-pulse; sleep 2
pactl load-module module-null-sink sink_name=tone sink_properties=device.description=ToneSink >/dev/null
pactl load-module module-remap-source master=tone.monitor source_name=testmic \
  source_properties=device.description=TestMic >/dev/null
pactl set-default-source testmic
start bash -c "while true; do pw-cat -p --target tone '$dir/tone.wav'; done"
# PipeWire's own test pattern: unlike pipewiresink mode=provide, it serves a
# consumer that reconnects.
pw-cli create-node adapter '{ factory.name=videotestsrc library.name=videotestsrc/libspa-videotestsrc
  node.name=tuxtestcam node.description=TuxTestCam media.class=Video/Source object.linger=true }' >/dev/null
echo "$dir/env.sh"
