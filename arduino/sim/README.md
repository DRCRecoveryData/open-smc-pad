# SMC-PAD emulator

Runs a sketch on your computer: the sketch, the platform's libraries (SMCPad, MIDIUSB,
ArduinoBLE, BLEMIDI, EEPROM) and the core are compiled **unchanged** for the host; only the
runtime underneath (`smcpad_rt.h`: LEDs, pads, buttons, knobs, time, MIDI, flash) is emulated
(`sim_runtime.cpp`). Change the firmware and the emulator shows the change: nothing here
knows what the sketch does.

What it is for: the animations in the guides (a scenario of gestures in, a GIF out, with fingers
on what is pressed), and testing sketch logic without the pad.

Two sketches in this repository run as they are: [`SMCPadStock`](../sketches/SMCPadStock) on the
full-size layout and [`PocketDemo`](../sketches/PocketDemo) on the Pocket's — the one in the
README animations. Any other sketch, GrooveBox included, takes the same place.

```python
import sys; sys.path.insert(0, "arduino/sim")
from scenario import Scenario, BUTTON

s = Scenario()                                      # the full-size board
s.caption("PAD BANK: the steps of the selected track").tap(BUTTON.PAD_BANK)
for pad in (12, 14, 8, 10):
    s.tap_pad(pad)
s.caption("PLAY").tap(BUTTON.PLAY).wait(1500)
frames, midi = s.make("arduino/sketches/SMCPadStock", "first-beat.gif")

s = Scenario(board="pocket")                        # the Pocket: 16 pads, no knobs or buttons
s.caption("Press harder: louder note, brighter pad").pad_down(6, 100, 300)
s.wait(500).pressure(6, 950).wait(500).pad_up(6)
frames, midi = s.make("arduino/sketches/PocketDemo", "pocket.gif")
```

`demo.py` renders the README animations (`docs/img/pocket-play.gif`,
`docs/img/arduino-demo.gif`) through that second path.

Needs `clang++` and Python with Pillow (`uv run --with pillow ...`). Time is virtual: a
scenario of a minute renders in seconds. `build.sh <sketch dir>` alone gives the emulator
program (`<scenario file> <ms> [fps]`, frames and outgoing MIDI as JSON lines on stdout; the
scenario format is at the top of `sim_runtime.cpp`).

Not emulated: Bluetooth centrals (the stack comes up, nobody connects), USB serial input,
the flash's timing.
