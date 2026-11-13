#!/usr/bin/env bash
# Bring-up helper for the ESP32-S3 USB-host bridge (docs/s3-bridge.md).
#
#   host/s3_bringup.sh [--build] [<serial port>]
#
# Checks the parts that need no pad and no working board: the host unit tests, the bridge
# firmware build (with --build, needs arduino-cli with esp32 3.3.12), and, when the S3 is
# plugged in and flashed, a live relay status. Then prints the manual hardware steps.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-python3}"
BUILD=0
PORT=""
for arg in "$@"; do
    case "$arg" in
        --build) BUILD=1 ;;
        -*) echo "!! unknown option $arg" >&2; exit 1 ;;
        *) PORT="$arg" ;;
    esac
done

step() { printf '\n== %s\n' "$*"; }
note() { printf '   %s\n' "$*"; }
ok()   { printf '   ok: %s\n' "$*"; }
bad()  { printf '   !! %s\n' "$*"; }

if ! "$PY" -c 'import serial' >/dev/null 2>&1 && [ -d /tmp/pserlib ]; then
    export PYTHONPATH="/tmp/pserlib${PYTHONPATH:+:$PYTHONPATH}"
fi

step "host unit tests"
"$PY" -m unittest discover -s "$ROOT/host/tests" -q
ok "frame codec and both tool backends"

if [ "$BUILD" = 1 ]; then
    step "bridge firmware build"
    ACLI="${ARDUINO_CLI:-$(command -v arduino-cli || true)}"
    if [ -z "$ACLI" ]; then
        bad "arduino-cli not found (set ARDUINO_CLI); the firmware needs arduino-esp32 3.3.12"
    else
        "$ACLI" compile --fqbn esp32:esp32:esp32s3 "$ROOT/hw/jl-s3-bridge" | tail -2
        ok "compiled with $ACLI"
    fi
fi

step "serial port"
if [ -z "$PORT" ]; then
    for candidate in /dev/serial/by-id/*CP210* /dev/serial/by-id/*CH343* \
                     /dev/serial/by-id/*USB_Single_Serial* /dev/ttyUSB* /dev/ttyACM*; do
        [ -e "$candidate" ] && { PORT="$candidate"; break; }
    done
fi
if [ -n "$PORT" ] && [ -e "$PORT" ]; then
    ok "$PORT"
else
    PORT=""
    bad "no USB-serial bridge port found (pass one as the first argument)"
fi

step "live status"
if [ -z "$PORT" ]; then
    note "skipped: no port"
else
    if "$PY" "$ROOT/host/s3_relay.py" --port "$PORT" status; then
        ok "the bridge answers HELLO"
    else
        bad "no answer: is hw/jl-s3-bridge flashed and the UART free (no daemon running)?"
    fi
fi

cat <<'EOF'

== manual steps (docs/s3-bridge.md has the details)

 1. pad off, mode midi           status shows pad: unknown, no bus drive
 2. pad on                       status shows pad: midi, vid 0x4353 pid 0x4b4d, cable counts
    optional                     host/s3_relay.py serve --midi, then host/midi_monitor.py
 3. mode uboot; keyer arm        power the pad on: armed -> keying -> ok, ROM 4C4A:2542
 4. read 64 KB                   jluboottool --device sock:/run/user/$UID/smcpad-s3.sock \
                                   "read 0x0 0x10000 /tmp/pad_64k.bin"
                                 (chip info prints on open) compare with the verified dump
 5. full backup + hash           restoring or flashing stays a manual decision
 6. VBUS, if needed              5 V from the devkit 5V pin to the pad (native port is in-only)
EOF
