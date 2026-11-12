"""Renders the README demos with the emulator (arduino/sim):

    python3 arduino/sim/demo.py

needs clang/clang++ and Pillow. Writes docs/img/pocket-play.gif (the README hero) and
docs/img/arduino-demo.gif (arduino/README.md), both drawing the Pocket: 16 pads, no knobs or
buttons, as arduino/sketches/PocketDemo plays them.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenario import Scenario  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
SKETCH = os.path.join(ROOT, "arduino", "sketches", "PocketDemo")
IMG = os.path.join(ROOT, "docs", "img")


def hold(s, pad, pressure=700, velocity=100, ms=500):
    """Presses a pad, leans on it, lets go; `pressure` may be a list of values over time."""
    s.pad_down(pad, velocity, pressure[0] if isinstance(pressure, list) else pressure)
    if isinstance(pressure, list):
        for p in pressure[1:]:
            s.wait(ms // len(pressure)).pressure(pad, p)
    s.wait(ms).pad_up(pad)
    return s


def play():
    """Four bars of the Pocket being played: the hero animation of the root README."""
    s = Scenario(board="pocket")
    s.caption("The Pocket, running this firmware in the emulator", read=800)
    s.caption("16 pads: a note, its velocity, its light", read=600)

    s.tap_pad(0, 55).tap_pad(5, 90).tap_pad(10, 75).tap_pad(15, 115)

    s.caption("Press harder: louder note, brighter pad", read=500)
    hold(s, 6, [250, 950], 110, 900)
    s.wait(300)

    s.caption("Held pressure is aftertouch", read=500)
    s.pad_down(1, 80, 300).pad_down(9, 100, 500).pad_down(14, 90, 700)
    s.wait(300).pressure(1, 900).wait(300).pressure(9, 1000).wait(300).pressure(14, 980)
    s.caption("Release: the note and the light fade", read=200)
    s.wait(500).pad_up(1).pad_up(9).pad_up(14)
    s.wait(1400)
    s.make(SKETCH, os.path.join(IMG, "pocket-play.gif"), fps=25)


def scale():
    """A scale up the 16 pads, then a chord held and squeezed: the animation of arduino/README."""
    s = Scenario(board="pocket")
    s.caption("SMCPad: every pad is a note and a light", read=700)
    for pad in range(16):
        s.tap_pad(pad, 50 + pad * 4, hold=110, gap=100)

    s.caption("How hard you hit it is the velocity", read=500)
    hold(s, 11, [200, 900], 120, 800)
    s.tap_pad(2, 100)
    s.wait(250)

    s.caption("Squeeze the pad: polyphonic aftertouch", read=500)
    s.pad_down(4, 90, 400).pad_down(9, 110, 600)
    s.wait(400).pressure(4, 950).wait(400).pressure(9, 1000).wait(500)
    s.caption("Let go: the note and the light go out together", read=200)
    s.wait(400).pad_up(4).pad_up(9)
    s.wait(1400)
    s.make(SKETCH, os.path.join(IMG, "arduino-demo.gif"), fps=25)


if __name__ == "__main__":
    play()
    scale()
