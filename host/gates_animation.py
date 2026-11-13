#!/usr/bin/env python3
"""Recreate the ill.Gates finger-drummer LED look on the SMC-PAD Pocket.

Source look (youtu.be/RMJNVMaHM7g, Garet Biglow): a 4x4 pad grid with cyan LED
rings, every ring showing a rotated bright arc, one orange ring accent and a blue
ring on the pad being hit.  The Pocket has a single LED per pad, so this keeps the
palette and the motion rather than the ring geometry:

  - cyan pad field with one white "arc glint" per pad, phases staggered across the
    grid so the glints travel around it like the rings' rotating arcs
  - one pad at a time carries the orange accent ring
  - a hit flashes white, holds blue, then fades back into the field
  - an auto groove plays hits on its own; real taps are followed as well

Talks to the MidiFighterSpectra sketch over raw MIDI (hw:2,0,0): channel 3 Note On
note 36+pad, velocity selects the palette colour; channel 4 note 0 forces bank 1.
Pad 0 is bottom-left, row by row upwards.
"""
import argparse
import math
import os
import sys
import time

NOTE_BASE = 36
CH_TRIG = 0x92
CH_CTRL = 0x93

# Spectra palette velocities (User Guide: 10 colours, dim then bright)
CYAN = 67           # 0x00FFFF
CYAN_DIM = 73       # 0x007F7F
BLUE = 79           # 0x0000FF
BLUE_DIM = 85       # 0x00007F
ORANGE = 19         # 0xF26722
ORANGE_DIM = 25     # 0x793311
WHITE = 115         # 0xFFFFFF

# Bottom-left, row by row upwards; the perimeter first, then the inner square.
SPIRAL = [0, 1, 2, 3, 7, 11, 15, 14, 13, 12, 8, 4, 5, 6, 10, 9]

# Auto groove, 16th steps at 104 BPM (two bars).  Pad = row * 4 + column.
STEP_BEATS = 0.144
KICK, SNARE, HAT = 0, 3, 7
GROOVE = [
    (0, KICK), (1, HAT), (2, 9), (3, HAT),
    (4, SNARE), (5, HAT), (6, KICK), (7, HAT),
    (8, 10), (9, HAT), (10, SNARE), (11, HAT),
    (12, KICK), (13, HAT), (14, 13), (15, HAT),
]

HIT_WHITE = 0.08    # s of white flash
HIT_BLUE = 0.30     # s of bright blue after the flash
HIT_FADE = 0.55     # s until the hit colour has fully faded

GLINT_PERIOD = 2.4  # s per pad between glints
GLINT_WIDTH = 0.13  # s the glint lasts
GLINT_STAGGER = 0.15

ACCENT_DWELL = 2.4  # s per pad with the orange ring


class Pad:
    def __init__(self, path):
        self.wfd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        self.rfd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        self.hit_at = [None] * 16
        self.held = [False] * 16
        self.rx = bytearray()

    def write(self, data):
        try:
            os.write(self.wfd, bytes(data))
        except BlockingIOError:
            pass

    def poll(self):
        try:
            chunk = os.read(self.rfd, 4096)
        except BlockingIOError:
            return
        except OSError:
            return
        self.rx.extend(chunk)
        while len(self.rx) >= 3:
            status, note, vel = self.rx[0], self.rx[1], self.rx[2]
            if status < 0x80 or status >= 0xF0:
                del self.rx[0]
                continue
            del self.rx[:3]
            if status in (0x82, CH_TRIG) and NOTE_BASE <= note < NOTE_BASE + 16:
                pad = note - NOTE_BASE
                if status == CH_TRIG and vel > 0:
                    self.hit_at[pad] = time.monotonic()
                    self.held[pad] = True
                else:
                    self.held[pad] = False


def frame(t, hit_at, held):
    """Colour per pad at time t (palette velocity, 0 = off)."""
    colors = [CYAN_DIM] * 16

    # slow diagonal shimmer: arcs' bright sides
    wave = math.sin(t * 1.5)
    for pad in range(16):
        row, col = divmod(pad, 4)
        if math.sin((row + col) * 1.1 - t * 2.2) > wave * 0.6:
            colors[pad] = CYAN

    # orange accent pad, walking the spiral
    colors[SPIRAL[int(t / ACCENT_DWELL) % 16]] = ORANGE

    # per-pad white arc glint, staggered like the rings' rotated bright arcs
    for pad in range(16):
        phase = (t + pad * GLINT_STAGGER) % GLINT_PERIOD
        if phase < GLINT_WIDTH:
            colors[pad] = WHITE

    # hits on top: white flash, blue hold, fade back
    now = time.monotonic()
    for pad in range(16):
        if colors[pad] == ORANGE:
            continue
        if held[pad]:
            colors[pad] = BLUE
            continue
        if hit_at[pad] is None:
            continue
        age = now - hit_at[pad]
        if age < HIT_WHITE:
            colors[pad] = WHITE
        elif age < HIT_BLUE:
            colors[pad] = BLUE
        elif age < HIT_FADE:
            colors[pad] = BLUE_DIM
    return colors


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--device", default="/dev/snd/midiC2D0")
    ap.add_argument("--seconds", type=float, default=45.0, help="run time (Ctrl+C stops)")
    ap.add_argument("--fps", type=float, default=50.0)
    ap.add_argument("--no-auto", action="store_true", help="only follow real taps")
    a = ap.parse_args()

    try:
        pad = Pad(a.device)
    except OSError as e:
        sys.exit(f"{a.device}: {e}")

    pad.write([CH_CTRL, 0x00, 0x7F])            # force bank 1
    sent = [None] * 16
    step = 0
    next_step = time.monotonic()
    beat = 1.0 / a.fps
    end = time.monotonic() + a.seconds
    print(f"running {a.seconds:.0f}s at {a.fps:.0f} fps on {a.device}; Ctrl+C stops")

    try:
        while time.monotonic() < end:
            t = time.monotonic()
            pad.poll()

            if not a.no_auto and t >= next_step:
                delays = 0
                while time.monotonic() >= next_step and delays < 4:
                    for slot, target in GROOVE:
                        if slot == step % 16:
                            pad.hit_at[target] = time.monotonic()
                    step += 1
                    next_step += STEP_BEATS
                    delays += 1

            colors = frame(t, pad.hit_at, pad.held)
            data = bytearray()
            for p in range(16):
                if colors[p] != sent[p]:
                    data += bytes([CH_TRIG, NOTE_BASE + p, colors[p]])
                    sent[p] = colors[p]
            if data:
                pad.write(data)

            now = time.monotonic()
            remaining = beat - (now - t)
            if remaining > 0:
                time.sleep(remaining)
    except KeyboardInterrupt:
        pass
    finally:
        for p in range(16):
            pad.write([CH_TRIG, NOTE_BASE + p, CYAN])
        print("done: pads left on the cyan field")


if __name__ == "__main__":
    main()
