#!/usr/bin/env python3
"""Run the pad LEDs through a stepped brightness ramp, for watching the chain.

    uv run --with pyserial host/led_ramp.py [--port /dev/ttyACM0] [--lead 15] [--step 1.6]

Needs the bring-up console on the pad: its `n a <rrggbb>` sets all 16 chain LEDs to one
brightness, which no sketch exposes. All 16 go through red, green, blue and white at
0x08 0x10 0x20 0x40 0x80 0xFF, then off. The script opens the port itself, so it needs
access to it (dialout group, or sudo); every command lands in work/led_ramp.log with a
wall-clock time, to compare with what was seen.
"""
import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: uv run --with pyserial host/led_ramp.py")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEVELS = (0x08, 0x10, 0x20, 0x40, 0x80, 0xFF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--lead", type=float, default=15.0, help="seconds of dark before the ramp")
    ap.add_argument("--step", type=float, default=1.6, help="seconds per level")
    ap.add_argument("--log", default=os.path.join(ROOT, "work", "led_ramp.log"))
    a = ap.parse_args()

    try:
        port = serial.Serial(a.port, 115200, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"{a.port}: {e} (dialout group, or run with sudo)")

    def send(cmd, secs):
        port.write(cmd.encode())
        end = time.time() + secs
        while time.time() < end:
            port.read(4096)

    def now():
        return time.strftime("%H:%M:%S")

    send("n\r", 0.3)
    time.sleep(a.lead)

    lines = []
    for name, ch in (("red", 0), ("green", 1), ("blue", 2)):
        for lv in LEVELS:
            c = [0, 0, 0]
            c[ch] = lv
            rgb = (c[0] << 16) | (c[1] << 8) | c[2]
            lines.append(f"{now()} {name} {lv:#04x}")
            send(f"n a {rgb:06x}\r", a.step)
    for lv in LEVELS:
        lines.append(f"{now()} white {lv:#04x}")
        send(f"n a {lv:02x}{lv:02x}{lv:02x}\r", a.step)
    send("n\r", 0.5)
    port.close()

    os.makedirs(os.path.dirname(a.log), exist_ok=True)
    with open(a.log, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
