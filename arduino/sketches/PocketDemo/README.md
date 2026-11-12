# PocketDemo

The Pocket on its own: 16 pressure pads playing a selectable key, with chords, latch, note
repeat and an arpeggiator. It is the sketch behind the README animations. There is no preset,
bank or Bluetooth logic: it plays straight out of USB-MIDI, and over BLE-MIDI when a host
listens.

- the pads walk the selected scale; each pad plays the chord on its scale degree, velocity
  from how hard it is hit, and the pressure opens the chord: light is the root, more the
  triad, hardest the seventh (CC 22 caps it; latched pads play the cap);
- latch turns a tap into a toggle; note repeat retriggers held and latched pads from the
  clock with rate, swing and tempo; the arpeggiator walks the merged notes of every
  held/latched pad up, down, up-down or random, one note per step, and can span 1 to 4
  octaves;
- holding a pad sends polyphonic aftertouch (`0xA0`) for every note of its chord, following
  the pressure (not while repeating or arpeggiating);
- the loop recorder captures the notes the pad plays — chords, repeats, arp steps — against
  the clock and repeats a 1 to 4 bar loop, with the top-row pads showing the position;
  another pass layers an overdub over the take and undo drops the last layer; a transpose CC
  moves the playback up or down without touching the take; the loop lives in RAM (lost on
  power cycle) and keeps the notes as played, so a later key change transposes
  live play but not the loop;
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
| 28 | clock: 0 internal (CC 26 tempo), 1 external 24-PPQN MIDI clock with start/stop, falling back to internal when it stops |
| 29 | arp octaves: 1..4 (the arp walk repeats the merged notes an octave up) |
| 30 | loop: 0 stop (the take is kept), 1 play, 2 record a new take (one loop length, then it plays), 3 clear, 4 overdub a new layer over the take, 5 undo the last layer (8 layers) |
| 31 | loop length: 1..4 bars, default 2 (changing it stops the loop) |
| 32 | loop transpose: 0..24 = -12..+12 semitones, 12 = as recorded (playback only, the take is untouched) |
| 33 | preset save: 0..7 stores the current state in a slot; program change 0..7 on any channel recalls it |
| 34 | loop store: stores the current take (events, length, layers, transpose) in flash |
| 35 | loop load: loads the stored take back, silent; CC 30 = 1 plays it |

A program change recalls the slot of that number: root, scale, chord cap, latch, repeat rate,
swing, tempo, arp, clock source and octave range. Empty slots are ignored. A stored loop take
survives a power cycle; until CC 34 is sent the take lives in RAM only.

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
