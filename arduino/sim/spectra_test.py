#!/usr/bin/env python3
"""Simulator tests for the Midi Fighter Spectra mode (arduino/sketches/MidiFighterSpectra).

Runs the sketch against the host emulator and checks the documented protocol only:
note/CC bytes, bank echo, colour bands, brightness/gate/pulse timing, clock sync and the
notes the Pocket must ignore.  No hardware needed:

    python3 arduino/sim/spectra_test.py
"""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenario import Scenario, BUTTON  # noqa: E402

SKETCH = os.path.abspath(os.path.join(HERE, '..', 'sketches', 'MidiFighterSpectra'))
PALETTE_H = os.path.join(SKETCH, 'SpectraPalette.h')

NOTE_CH = 0x92          # channel 3: triggers and colour notes
CTRL_CH = 0x93          # channel 4: CC, bank, animations, ground LEDs
SIDE_CH = 0x94          # channel 5: side-button CCs

failures = []
checks = [0]


def check(name, ok, detail=''):
    checks[0] += 1
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', name, '' if ok else ': ' + str(detail)))
    if not ok:
        failures.append(name)


def messages(midi):
    return [tuple(m['midi']) for m in midi]


def palette():
    text = open(PALETTE_H).read()
    body = text.split('SPECTRA_PALETTE[SPECTRA_PALETTE_SIZE] = {', 1)[1].split('};', 1)[0]
    return [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{6})u', body)]


def scale(rgb, gain):
    r = (((rgb >> 16) & 0xFF) * gain) // 255
    g = (((rgb >> 8) & 0xFF) * gain) // 255
    b = ((rgb & 0xFF) * gain) // 255
    return (r << 16) | (g << 8) | b


def transition_times(frames, pad, on=True):
    """Times at which the pad's ring becomes `on` (any non-zero colour)."""
    times, state = [], None
    for f in frames:
        now = f['pads'][pad] != 0
        if state is not None and now != state and now == on:
            times.append(f['t'])
        state = now
    return times


def periods(times):
    return [b - a for a, b in zip(times, times[1:])]


def load_frames(text):
    frames, midi = [], []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        (frames if 'pads' in obj else midi).append(obj)
    return frames, midi


def build_variant(flags, out):
    env = dict(os.environ, EXTRA_CXXFLAGS=flags)
    subprocess.run([os.path.join(HERE, 'build.sh'), SKETCH, out], check=True,
                   capture_output=True, env=env)
    return out


def run_exe(exe, scenario, duration, fps=25):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write(scenario.text())
        path = f.name
    try:
        out = subprocess.run([exe, path, str(duration), str(fps)],
                             capture_output=True, text=True, check=True)
        return load_frames(out.stdout)
    finally:
        os.unlink(path)


# ---------------------------------------------------------------- tests

def test_idle():
    frames, midi = Scenario().run(SKETCH, 200)
    check('idle: no MIDI', not midi, messages(midi))
    check('idle: rings off (inactive colour)', frames[-1]['pads'] == [0] * 16,
          frames[-1]['pads'])
    check('idle: button LEDs off', frames[-1]['leds'] == [0] * 10, frames[-1]['leds'])


def test_press_velocity():
    s = Scenario()
    s.pad_down(0, 20).wait(50).pad_up(0).wait(50)
    s.pad_down(15, 1).wait(50).pad_up(15).wait(50)
    _, midi = s.run(SKETCH, 400)
    got = messages(midi)
    want = [(NOTE_CH, 36, 127), (0x82, 36, 0), (NOTE_CH, 51, 127), (0x82, 51, 0)]
    check('press: note map, fixed velocity, release', got == want, got)


def test_velocity_ignored():
    s = Scenario()
    s.pad_down(8, 1).wait(40).pad_up(8).wait(40)
    s.pad_down(8, 127, pressure=1000).wait(40).pad_up(8).wait(40)
    _, midi = s.run(SKETCH, 300)
    ons = [m for m in messages(midi) if m[0] == NOTE_CH]
    check('press: pad velocity is ignored (FSR pads)', ons == [(NOTE_CH, 44, 127)] * 2, ons)


def test_momentary_cc():
    exe = build_variant('-DSPECTRA_MOMENTARY_CC=1', '/tmp/smcpad-sim-spectra-cc')
    s = Scenario()
    s.pad_down(4, 100).wait(60).pad_up(4).wait(60)
    frames, midi = run_exe(exe, s, 300, 25)
    got = messages(midi)
    want = [(NOTE_CH, 40, 127), (0xB3, 40, 127), (0x82, 40, 0), (0xB3, 40, 0)]
    check('momentary CC: note + CC press/release on ch4', got == want, got)


def test_bank_force():
    s = Scenario()
    s.wait(50).midi(CTRL_CH, 0x01, 0x7F).wait(80)    # bank 2 (C#-1)
    s.midi(CTRL_CH, 0x02, 0x7F).wait(80)             # bank 3 (D-1)
    s.pad_down(0).wait(50).pad_up(0).wait(50)
    _, midi = s.run(SKETCH, 500)
    got = messages(midi)
    want = [(0x83, 0, 0), (CTRL_CH, 1, 127),         # bank 1 -> 2 echo: Off C-1, On C#-1
            (0x83, 1, 0), (CTRL_CH, 2, 127),         # bank 2 -> 3 echo
            (NOTE_CH, 68, 127), (0x82, 68, 0)]       # bank 3 note for pad 0
    check('bank: ch4 notes 0..3 force the bank and echo', got == want, got)


def test_bank_change_releases_held():
    s = Scenario()
    s.pad_down(3).wait(50)
    s.midi(CTRL_CH, 1, 0x7F).wait(80)                # force bank 2 while pad 3 is held
    s.pad_up(3).wait(80)
    s.pad_down(3).wait(50).pad_up(3).wait(50)
    _, midi = s.run(SKETCH, 500)
    got = messages(midi)
    want = [(NOTE_CH, 39, 127),
            (0x82, 39, 0), (0x83, 0, 0), (CTRL_CH, 1, 127),
            (NOTE_CH, 55, 127), (0x82, 55, 0)]
    check('bank: held pads released with the old bank note', got == want, got)


def test_colour_bands():
    pal = palette()
    check('palette: guide example velocity 7 = bright red', pal[1] == 0xED1F24, hex(pal[1]))
    check('palette: velocity 1-6 is the black dim-white swatch', pal[0] == 0x000000, hex(pal[0]))

    s = Scenario()
    sends = []
    for band in range(20):
        sends.append(s.t)
        s.midi(NOTE_CH, 36, band * 6 + 1).wait(80)
    frames, _ = s.run(SKETCH, s.t + 100, 50)
    ok = True
    for at, band in zip(sends, range(20)):
        window = [f['pads'][0] for f in frames if at + 20 <= f['t'] <= at + 60]
        if not window or any(c != pal[band] for c in window):
            ok = False
            check('colour band %d (velocity %d)' % (band, band * 6 + 1), False,
                  window[:3])
    check('colour: all 20 velocity bands', ok)

    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(80)          # bright red
    s.midi(NOTE_CH, 36, 0).wait(80)                   # MIDI colour off
    s.midi(NOTE_CH, 36, 7).wait(80)
    s.midi(NOTE_CH, 36, 121).wait(80)                 # force Utility active colour
    frames, _ = s.run(SKETCH, s.t + 100, 50)

    def pads_in(a, b):
        return [f['pads'][0] for f in frames if a <= f['t'] <= b]

    check('colour: override applies', pads_in(60, 110) == [0xED1F24] * len(pads_in(60, 110)),
          pads_in(60, 110)[:3])
    check('colour: velocity 0 returns to Utility inactive', pads_in(140, 190) == [0] * len(pads_in(140, 190)),
          pads_in(140, 190)[:3])
    check('colour: 121..127 forces the active colour', pads_in(300, 350) == [0xFFFFFF] * len(pads_in(300, 350)),
          pads_in(300, 350)[:3])


def test_brightness():
    lut = [0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255]
    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)          # bright red
    sends = []
    for level in range(16):
        sends.append(s.t)
        s.midi(CTRL_CH, 36, 18 + level).wait(80)
    frames, _ = s.run(SKETCH, s.t + 100, 50)
    ok = True
    for at, level in zip(sends, range(16)):
        want = scale(0xED1F24, lut[level])
        window = [f['pads'][0] for f in frames if at + 20 <= f['t'] <= at + 60]
        if not window or any(c != want for c in window):
            ok = False
            check('brightness level %d' % level, False, (want, window[:3]))
    check('brightness: 16 levels, 0 off / 15 full', ok)


def test_gate_timing():
    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)          # bright red
    s.midi(CTRL_CH, 36, 38).wait(2400)                # gate rate 1 = one cycle per beat
    frames, _ = s.run(SKETCH, s.t, 200)
    per = periods(transition_times(frames, 0))[1:]   # the first cycle starts mid-phase
    check('gate 1: ~500 ms period at 120 BPM',
          len(per) >= 3 and all(abs(p - 500) < 40 for p in per), per)

    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)
    s.midi(CTRL_CH, 36, 37).wait(2400)                # gate rate 2
    frames, _ = s.run(SKETCH, s.t, 200)
    per = periods(transition_times(frames, 0))[1:]
    check('gate 2: ~250 ms period at 120 BPM',
          len(per) >= 5 and all(abs(p - 250) < 40 for p in per), per)

    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)
    s.midi(CTRL_CH, 36, 34).wait(1000)                # gate rate 16: ~31 ms cycle
    frames, _ = s.run(SKETCH, s.t, 400)
    transitions = transition_times(frames, 0) + transition_times(frames, 0, on=False)
    check('gate 16: fast flashing (many transitions in 1 s)', len(transitions) >= 20,
          len(transitions))


def test_gate_clock():
    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)
    s.midi(CTRL_CH, 36, 38)
    for _ in range(100):                              # 10 ms clock = 240 ms beat
        s.midi(0xF8).wait(10)
    s.wait(3000)                                      # clock stops: back to 500 ms
    frames, _ = s.run(SKETCH, s.t + 100, 200)
    on = transition_times(frames, 0)
    per = [(p, t) for p, t in zip(periods(on), on[1:])]
    early = [p for p, t in per if 400 < t < 1000]
    check('clock: gate follows MIDI clock (~240 ms)',
          len(early) >= 2 and all(abs(p - 240) < 45 for p in early), early)
    late = [p for p, t in per if t > 2400]  # one cycle to settle after the clock stops
    check('clock: after 500 ms silence back to 500 ms beat',
          len(late) >= 2 and all(abs(p - 500) < 45 for p in late), late)


def test_pulse():
    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)
    s.midi(CTRL_CH, 36, 47).wait(900)                 # pulse rate 1
    frames, _ = s.run(SKETCH, s.t, 200)
    values = [f['pads'][0] for f in frames if f['t'] > 120]
    bright = [(v >> 16) & 0xFF for v in values]
    check('pulse: full and zero and intermediate brightness',
          max(bright) >= 230 and min(bright) <= 12 and any(20 < b < 200 for b in bright),
          (min(bright), max(bright)))

    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)
    s.midi(CTRL_CH, 36, 50).wait(300)                 # > 49: animation off
    frames, _ = s.run(SKETCH, s.t, 100)
    values = {f['pads'][0] for f in frames if f['t'] > 120}
    check('pulse: velocity 50 clears the animation', values == {0xED1F24}, values)


def test_colour_and_animation():
    s = Scenario()
    s.wait(40).midi(NOTE_CH, 36, 7).wait(40)          # bright red
    s.midi(CTRL_CH, 36, 39).wait(3300)                # gate 1/2 = 1 s cycle
    frames, _ = s.run(SKETCH, s.t, 100)
    per = periods(transition_times(frames, 0))
    colors = {f['pads'][0] for f in frames if f['t'] > 200}
    check('combined: colour 7 with gate 1/2 (1 s)',
          colors == {0, 0xED1F24} and len(per) >= 2 and all(abs(p - 1000) < 60 for p in per),
          (colors, per))


def test_side_inputs_ignored():
    s = Scenario()
    s.wait(40)
    for note in (20, 25, 38, 43):                     # side-button notes (outputs, not inputs)
        s.midi(CTRL_CH, note, 127).wait(20)
    s.midi(SIDE_CH, 20, 127).wait(20)                 # side-button CC
    s.pad_down(0).wait(50).pad_up(0).wait(50)
    frames, midi = s.run(SKETCH, 400)
    got = messages(midi)
    check('side inputs: ignored (no MIDI)', got == [(NOTE_CH, 36, 127), (0x82, 36, 0)], got)
    check('side inputs: no LED change',
          frames[-1]['pads'] == [0] * 16 and frames[-1]['leds'] == [0] * 10,
          (frames[-1]['pads'], frames[-1]['leds']))


def test_side_notes():
    s = Scenario()
    for b in (BUTTON.PAD_BANK, BUTTON.KNOB_BANK, BUTTON.PLAY, BUTTON.STOP):
        s.press(b).wait(40).release(b).wait(40)
    frames, midi = s.run(SKETCH, s.t + 100, 50)
    want = [(CTRL_CH, 22, 127), (0x83, 22, 0),        # Left1
            (CTRL_CH, 20, 127), (0x83, 20, 0),        # Left3
            (CTRL_CH, 25, 127), (0x83, 25, 0),        # Right1
            (CTRL_CH, 23, 127), (0x83, 23, 0)]        # Right3
    check('side: note buttons send the Appendix 1 notes', messages(midi) == want, messages(midi))
    check('side: side-button LEDs stay off', not any(frames[-1]['leds'][1:7]),
          frames[-1]['leds'])


def test_side_bank_buttons():
    s = Scenario()
    s.press(BUTTON.LEFT).wait(40).release(BUTTON.LEFT).wait(40)      # previous: 1 -> 4
    s.press(BUTTON.RIGHT).wait(40).release(BUTTON.RIGHT).wait(40)    # next: 4 -> 1
    _, midi = s.run(SKETCH, s.t + 100, 50)
    want = [(0x83, 0, 0), (CTRL_CH, 3, 127),
            (0x83, 3, 0), (CTRL_CH, 0, 127)]
    check('side: < / > change bank with wrap and echo', messages(midi) == want, messages(midi))


def test_side_per_bank():
    s = Scenario()
    s.wait(20).midi(CTRL_CH, 1, 0x7F).wait(40)                        # bank 2
    s.press(BUTTON.PAD_BANK).wait(40).release(BUTTON.PAD_BANK).wait(40)
    _, midi = s.run(SKETCH, s.t + 100, 50)
    want = [(0x83, 0, 0), (CTRL_CH, 1, 127), (CTRL_CH, 28, 127), (0x83, 28, 0)]
    check('side: notes follow the bank', messages(midi) == want, messages(midi))

    exe = build_variant('-DSPECTRA_SIDE_BANK_BUTTONS=0',
                        '/tmp/smcpad-sim-spectra-side-bankoff')
    s = Scenario()
    s.wait(20).midi(CTRL_CH, 1, 0x7F).wait(40)
    s.press(BUTTON.PAD_BANK).wait(40).release(BUTTON.PAD_BANK).wait(40)
    _, midi = run_exe(exe, s, s.t + 100, 50)
    want = [(0x83, 0, 0), (CTRL_CH, 1, 127), (CTRL_CH, 22, 127), (0x83, 22, 0)]
    check('side: Bank Side Buttons off keeps the bank-1 numbers',
          messages(midi) == want, messages(midi))


def test_side_hold_across_bank():
    s = Scenario()
    s.press(BUTTON.PLAY).wait(40)
    s.midi(CTRL_CH, 1, 0x7F).wait(40)                                 # bank 2 while held
    s.release(BUTTON.PLAY).wait(40)
    _, midi = s.run(SKETCH, s.t + 100, 50)
    want = [(CTRL_CH, 25, 127), (0x83, 0, 0), (CTRL_CH, 1, 127), (0x83, 25, 0)]
    check('side: release after a bank change uses the held number',
          messages(midi) == want, messages(midi))


def test_side_cc_mode():
    exe = build_variant('-DSPECTRA_SIDE_MODE=2', '/tmp/smcpad-sim-spectra-side-cc')
    s = Scenario()
    s.press(BUTTON.PAD_BANK).wait(40).release(BUTTON.PAD_BANK).wait(40)
    _, midi = run_exe(exe, s, s.t + 100, 50)
    want = [(0xB4, 22, 127), (0xB4, 22, 0)]
    check('side: CC mode uses channel 5', messages(midi) == want, messages(midi))


def test_side_all_notes():
    exe = build_variant('-DSPECTRA_SIDE_MODE=1', '/tmp/smcpad-sim-spectra-side-notes')
    s = Scenario()
    s.press(BUTTON.LEFT).wait(40).release(BUTTON.LEFT).wait(40)
    _, midi = run_exe(exe, s, s.t + 100, 50)
    want = [(CTRL_CH, 21, 127), (0x83, 21, 0)]
    check('side: all-note mode sends Left2 without changing bank',
          messages(midi) == want, messages(midi))


def test_ground_leds():
    s = Scenario()
    sends = []
    for note, velocity in ((16, 127), (16, 18), (16, 21), (16, 50),
                           (19, 127), (19, 0), (17, 127)):
        sends.append((s.t, note))
        s.midi(CTRL_CH, note, velocity).wait(80)
    frames, _ = s.run(SKETCH, s.t + 100, 50)

    def window(at):
        return [f['leds'] for f in frames if at + 20 <= f['t'] <= at + 60]

    cases = [('127 turns LED 1 on', 0, 1), ('velocity 18 is level 0 (off)', 0, 0),
             ('velocity 21 is level 3 (on)', 0, 1), ('velocity 50 is off', 0, 0),
             ('LED 4 is the NOTE REPEAT LED', 9, 1), ('velocity 0 turns it off', 9, 0),
             ('LED 2 is the REC LED', 7, 1)]
    for (name, index, value), (at, _) in zip(cases, sends):
        w = window(at)
        check('ground: ' + name, w and all(leds[index] == value for leds in w), w)


def test_ground_animations():
    s = Scenario()
    s.wait(40).midi(CTRL_CH, 16, 38).wait(2400)       # gate, one cycle per beat
    frames, _ = s.run(SKETCH, s.t, 50)
    values = [f['leds'][0] for f in frames if f['t'] > 200]
    check('ground: gate blinks', set(values) == {0, 1}, values[:24])

    s = Scenario()
    s.wait(40).midi(CTRL_CH, 16, 47).wait(2400)       # pulse, one cycle per beat
    frames, _ = s.run(SKETCH, s.t, 50)
    values = [f['leds'][0] for f in frames if f['t'] > 200]
    check('ground: pulse blinks', set(values) == {0, 1}, values[:24])


def test_stress():
    s = Scenario()
    ons = 0
    for i in range(300):
        pad = i % 16
        s.pad_down(pad, 10 + i % 117).wait(12).pad_up(pad).wait(8)
        if i % 3 == 0:
            s.midi(0xF8)
        if i % 50 == 25:
            s.midi(CTRL_CH, (i // 50) % 4, 0x7F)
        ons += 1
    frames, midi = s.run(SKETCH, s.t + 200, 25)
    got = messages(midi)
    downs = [m for m in got if m[0] == NOTE_CH]
    ups = [m for m in got if m[0] == 0x82]
    check('stress: every press has an on and an off',
          len(downs) == ons and len(ups) == ons, (len(downs), len(ups), ons))
    check('stress: frames rendered to the end', frames and frames[-1]['t'] > 4000,
          frames[-1]['t'] if frames else None)


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_')]
    for test in tests:
        test()
    print('\n%d/%d checks passed' % (checks[0] - len(failures), checks[0]))
    if failures:
        print('failed: ' + ', '.join(failures))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
