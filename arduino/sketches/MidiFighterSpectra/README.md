# MidiFighterSpectra

The Pocket speaking the Midi Fighter Spectra MIDI protocol (User Guide Ver 1.01), as
documented — a clean-room implementation, not DJ TechTools' firmware. 16 pads, four virtual
banks, MIDI-controlled ring colours and animations, the 6 side buttons and 4 ground LEDs
(mapped onto the Pocket's buttons). USB-MIDI only (the Spectra has no BLE).

Build and flash like any sketch:

    host/build_sketch.sh arduino/sketches/MidiFighterSpectra            # work/sketch/.../bin
    host/build_sketch.sh arduino/sketches/MidiFighterSpectra --flash    # with the pad attached

Test in the host emulator (no hardware, no Python image libraries):

    python3 arduino/sim/spectra_test.py

## MIDI map

Channels are 1-based as in the guide; base channel 3 gives status bytes `0x92` (triggers,
colours), `0x93` (bank notes, animations, ground LEDs, momentary CC), `0xB3` (momentary CC)
and `0xB4` (side-button CCs).

### Triggers — channel 3 Note On/Off

Fixed velocity 127 (the Spectra's triggers are not velocity sensitive; pad pressure is
ignored). Note = 36 + 16×bank + pad, pad 0 = bottom-left:

| Bank | Notes | Bank force (ch4 Note On) |
|------|-------|--------------------------|
| 1 | 36..51 | 0 (C-1) |
| 2 | 52..67 | 1 (C#-1) |
| 3 | 68..83 | 2 (D-1) |
| 4 | 84..99 | 3 (D#-1) |

A bank force echoes the change on channel 4: Note Off for the old bank's note, then Note On
(127) for the new one. Boot bank is 1. Notes 20..43 on channel 4 are side-button notes and are
ignored; the guide's prose example ("C-1 forces bank 2") contradicts its own table and is
treated as a typo — the table above is used.

### Colours — channel 3 Note On with the trigger's own pitch

| Velocity | Effect |
|----------|--------|
| 0 | MIDI colour off: Utility active/inactive colours |
| 1..120 | palette band `(velocity-1)/6`: 10 colours × dim/bright |
| 121..127 | force the Utility active colour, pressed or not |

`SpectraPalette.h` is generated from Fig 2 of the guide (`tools/extract_spectra_palette.py`):
the bright entries are the swatch fills exactly, dim entries are 50 % of them, and the 1–6
band is black (the guide's dim white). Velocity 7 is bright red, as in the guide's example.

### Animations — channel 4 Note On with the trigger's own pitch

| Velocity | Effect |
|----------|--------|
| < 18 | none |
| 18..33 | brightness level 0..15 (0 = off, 15 = full) |
| 34..41 | gate, rate index 0..7: 16, 8, 4, 2, 1, ½, ¼, ⅛ cycles per beat |
| 42..49 | pulse, rate index 0..7: 32, 16, 8, 4, 2, 1, ½, ⅛ cycles per beat |
| 50..127 | none |

Colour and animation combine (e.g. vel 7 then 39: red, gated at ½ cycle per beat). Gate is on
for the first half of a cycle; pulse is a raised cosine. All pads share one phase, so gates
line up. Timing follows the incoming MIDI clock (`0xF8`, 24 ppqn); `0xFA` resets the phase,
`0xFB`/`0xFC` are ignored, and after 500 ms without clock ticks the beat falls back to 500 ms
(120 BPM). "Cycles per beat" is our reading of the guide's rate table.

## Side buttons and ground LEDs

The Pocket's 10 buttons stand in for the Spectra's 6 side buttons and 4 ground LEDs. The
Spectra's side buttons have no LEDs, so their Pocket LEDs stay off; the four remaining button
LEDs are the ground LEDs.

| Spectra button | Pocket button | Default action | Note, banks 1..4 (ch4) |
|----------------|---------------|----------------|------------------------|
| Left Side 1 | PAD BANK | Note | 22, 28, 34, 40 |
| Left Side 2 | `<` | Previous Bank | — |
| Left Side 3 | KNOB BANK | Note | 20, 26, 32, 38 |
| Right Side 1 | PLAY | Note | 25, 31, 37, 43 |
| Right Side 2 | `>` | Next Bank | — |
| Right Side 3 | STOP | Note | 23, 29, 35, 41 |

Side notes are sent on channel 4, CCs on channel 5 (`0xB4`, 127 on press / 0 on release). The
Utility's "Bank Side Buttons" per-bank numbering is on; previous/next wrap (1→4 and 4→1) and
send the same bank echo as a ch4 bank note. `SpectraConfig.h` replaces the Utility:
`SPECTRA_SIDE_MODE` 0 = factory defaults above, 1 = all six send notes, 2 = all six send CCs;
`SPECTRA_SIDE_BANK_BUTTONS` 0 keeps the bank-1 numbers in every bank.

Ground LEDs respond to ch4 notes 16/17/18/19 on the BT, REC, SHIFT and NOTE REPEAT LEDs, with
the same velocity rules as the rings. The Pocket's LEDs are binary and coloured rather than
dimmable white: brightness collapses to on/off (18 = off), gate and pulse to blinking, and
127 is steady on.

## In Ableton (or any host)

The rings are driven by the same MIDI a real Spectra receives; Ableton itself has no
Spectra support, so you send these notes from a clip or a Max for Live device:

1. **Preferences → Link/Tempo/MIDI → MIDI Ports**: enable **Track** on the pad's output, and
   **Sync** too if you want the animations to follow Live's clock.
2. A MIDI track with **MIDI To = the pad**, channel **3**, sends colours: notes are the
   current bank's trigger numbers (36–51 in bank 1, 52–67 in bank 2, …) and the velocity
   picks the band — 1–6 black, **7 bright red**, then each 6-velocity step moves through the
   10-colour palette; 121–127 forces the active white.
3. A second track (clips carry no per-note channel) to channel **4**, same notes, sends
   animations: 18–33 brightness 0–15, 34–41 gate, 42–49 pulse, ≥ 50 off. Colour and
   animation stack on a pad.
4. Notes 0–3 on channel 4 (C-1…D#-1) force banks 1–4; the pad echoes the old/new bank notes
   back on channel 4.

With **Sync** enabled the animations run from Live's 24-ppqn clock; without it they free-run
at the 120 BPM fallback. Pad presses also reach Live as notes, so routing the same track
back to the pad can light rings from what you play (and can feed back if monitored — route
deliberately). Only Note On is decoded for LEDs; ordinary Note Offs are ignored, as on the
Spectra.

## Divergences from a Spectra

These are hardware or undocumented-software limits, documented rather than emulated:

- no Utility app and no "Super Combo" patterns (the guide calls the combos secret): side
  actions, colours, spark/wave/sleep and the rest are compile-time in `SpectraConfig.h`;
- the 4 ground LEDs are the Pocket's binary coloured button LEDs, not dimmable white lights;
  the 6 side-button LEDs stay off, as on the Spectra;
- side CC release (0), bank wrap and ground velocities 50..126 (off) are assumptions where
  the guide is silent;
- pads are FSR pressure pads: output velocity is fixed at 127 as the Spectra does;
- USB VID/PID and product string stay the SMC-PAD's; no DJ TechTools identity;
- no BLE, no aftertouch, no velocity sensitivity.

## Assumptions where the guide is silent

- Held pads are released with their old-bank notes before a bank change, so the host never
  sees stuck notes.
- The bank change is echoed only when the bank actually changes.
- MIDI colours and animations are per pad, shared by all banks; nothing is persisted (RAM).
- Default Utility colours: active white, inactive off (`spectraActiveColor` /
  `spectraInactiveColor` in `SpectraConfig.h`).
- Spark and the bank-change wave are on (40 ms white flash per pressed pad, ~250 ms white
  sweep); they are approximations of the Utility animations, which are not documented
  frame-exactly. Sleep is off (`SPECTRA_SLEEP_MIN`).

## Verification

- `python3 arduino/sim/spectra_test.py` — 44/44 checks: note map and fixed velocity, release
  pairing, moment CC variant (`-DSPECTRA_MOMENTARY_CC=1`), bank force/echo and held-note
  release, all 20 colour bands, 16 brightness levels, gate/pulse timing at 120 BPM and from a
  24-ppqn clock, clock loss fallback, side-button notes/bank wrap/per-bank numbering/CC and
  all-note variants, ground LEDs (on/off/brightness/gate/pulse), ignored side-note inputs,
  300-press stress.
- `host/build_sketch.sh arduino/sketches/MidiFighterSpectra` — 149128 bytes (35 % of the
  425984-byte app area).
- On the pad (needs approval): `amidi -d` should show `92 24 7F` for the bottom-left pad,
  bank force should echo `83 <old-note> 00` / `93 <new-note> 7F`, colour vel 7 should be
  bright red, PAD BANK/KNOB BANK/PLAY/STOP should send `93 16/14/19/17 7F`, ch4 notes 16–19
  should light the four button LEDs, and gate rates should follow the host clock.
