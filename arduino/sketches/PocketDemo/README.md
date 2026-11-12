# PocketDemo

The Pocket on its own: 16 pads, one note each, played by hand.

- pad *n* sends Note On/Off for note `48 + n` on channel 1, velocity from how hard it is hit;
- holding a pad sends polyphonic aftertouch (`0xA0`) that follows the pressure;
- the pad's LED is its colour around the wheel; pressure brightens it towards white, and a
  released pad fades out like the note it played.

It is the sketch behind the README animations. There is no preset, bank or Bluetooth logic: it
plays straight out of USB-MIDI.

## On the pad

Upload it like any sketch: open `PocketDemo.ino`, choose **M-Vave SMC-PAD (open firmware)** and
the pad's port, Upload (one-time bootloader install: [the top README](../../../README.md#install)).

## In the emulator

From the repository root (needs `clang++` and Pillow, as [the emulator](../../sim/README.md)
explains), `scenario.py` drives the gestures and renders the result:

```python
import sys; sys.path.insert(0, "arduino/sim")
from scenario import Scenario

s = Scenario(board="pocket")
s.caption("Press harder: louder note, brighter pad", read=600)
s.pad_down(6, 100, 300).wait(500).pressure(6, 950).wait(500).pad_up(6).wait(800)
s.make("arduino/sketches/PocketDemo", "pocket.png", at=800)   # a still
s.make("arduino/sketches/PocketDemo", "pocket.gif")           # or the whole run, with fingers
```

`python3 arduino/sim/demo.py` renders the two README animations,
[`docs/img/pocket-play.gif`](../../../docs/img/pocket-play.gif) and
[`docs/img/arduino-demo.gif`](../../../docs/img/arduino-demo.gif), through the same path.
