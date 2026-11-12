# PocketDemo

The Pocket on its own: 16 pressure pads playing a selectable key, with chords, latch, note
repeat and an arpeggiator. It is the sketch behind the README animations. There is no preset,
bank or Bluetooth logic: it plays straight out of USB-MIDI, and over BLE-MIDI when a host
listens.

- the pads walk the selected scale; each pad plays the chord on its scale degree, velocity
  from how hard it is hit, and the pressure opens the chord: light is the root, more the
  triad, hardest the seventh (CC 22 caps it; latched pads play the cap);
- latch turns a tap into a toggle; note repeat retriggers held and latched pads from an
  internal clock with rate, swing and tempo; the arpeggiator walks the merged notes of every
  held/latched pad up, down, up-down or random, one note per step;
- holding a pad sends polyphonic aftertouch (`0xA0`) for every note of its chord, following
  the pressure (not while repeating or arpeggiating);
- the pad's LED is its colour around the wheel: root pads sit brighter towards white,
  pressure brightens a pad, a repeat or arp step flashes the pads the note came from, and a
  released pad fades out.

## MIDI CCs

On any channel, from USB or BLE. Everything is kept in flash.

| CC | Meaning |
|----|---------|
| 20 | root: 0..11 = C..B |
| 21 | scale: 0 chromatic, 1 major pentatonic, 2 minor pentatonic, 3 major, 4 natural minor, 5 dorian, 6 mixolydian, 7 blues |
| 22 | chord voice cap: 0 single note, 1 triad, 2 seventh |
| 23 | latch: 0 off, 1 a tap toggles a pad |
| 24 | note repeat: 0 off, 1..8 = 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T |
| 25 | swing: 0..4 |
| 26 | tempo: 0..127 = 30..300 BPM |
| 27 | arp: 0 whole chord, 1 up, 2 down, 3 up-down, 4 random |

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
