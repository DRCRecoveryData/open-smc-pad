// The Pocket on its own: 16 pads played by hand, in a selectable key.
//   - the pads walk the selected scale: pad 1 is the root, the next pads the next
//     scale notes, on up through the octaves
//   - each pad plays the chord of its scale degree, stacked from the scale's degrees;
//     the pressure opens it: light is the root, more the triad, hardest the seventh;
//     CC 22 caps the voice (0 single, 1 triad, 2 seventh) and latched pads play the cap
//   - MIDI CC 20 picks the root (0..11 = C..B), CC 21 the scale (0 chromatic,
//     1 major pentatonic, 2 minor pentatonic, 3 major, 4 natural minor, 5 dorian,
//     6 mixolydian, 7 blues), on any channel, from USB or BLE; the choices are kept in flash
//   - latch (CC 23: 0 off, 1 on): a tap toggles a pad on or off instead of momentary
//   - note repeat (CC 24: 0 off, 1..8 = 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T):
//     held and latched pads retrigger from the clock, with a 50% gate; CC 25 picks
//     the swing (0..4 = 50..66%) and CC 26 the tempo (0..127 = 30..300 BPM)
//   - arpeggiator (CC 27: 0 whole chord, 1 up, 2 down, 3 up-down, 4 random): the notes of
//     all held/latched pads merge into one sorted sequence and walk it, one note per repeat
//     step; CC 29 spans the walk over 1..4 octaves (the merged notes repeat an octave up);
//     each step flashes the pad(s) the note came from
//   - clock: CC 28 = 0 runs the repeat/arp from the internal clock (CC 26 tempo), 1 follows
//     an external 24-PPQN MIDI clock (start/continue/stop honoured) and falls back to the
//     internal clock when the host's clock stops
//   - loop recorder (CC 30: 0 stop, 1 play, 2 record, 3 clear; CC 31: 1..4 bars): it records
//     the notes the sketch plays against the clock and repeats them; the top-row pads show
//     the position while it runs (the loop is in RAM, lost on power cycle)
//   - each pad sends Note On/Off on channel 1, velocity from how hard it is hit
//   - holding a pad sends polyphonic aftertouch (0xA0) for every note of its chord,
//     following the pressure (repeating pads are articulated by their retriggers instead)
//   - everything goes out over USB-MIDI and, when a host listens, BLE-MIDI (untethered)
//   - the pad LED is the pad's colour; root pads sit brighter towards white, pressure
//     brightens a pad towards white, a repeat or arp step flashes it, and a released pad
//     fades out like the notes it played
#include <SMCPad.h>
#include <MIDIUSB.h>
#include <BLEMIDI.h>
#include <EEPROM.h>

static const uint8_t CH = 0;                         // MIDI channel 1
static const uint8_t BASE_NOTE = 48;                 // C3 is pad 1 of a C scale

struct Scale {
  const char *name;
  uint8_t count;
  uint8_t step[12];                                  // semitones above the root
};

static const Scale SCALES[] = {
  { "Chromatic",        12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
  { "Major pentatonic",  5, { 0, 2, 4, 7, 9 } },
  { "Minor pentatonic",  5, { 0, 3, 5, 7, 10 } },
  { "Major",             7, { 0, 2, 4, 5, 7, 9, 11 } },
  { "Natural minor",     7, { 0, 2, 3, 5, 7, 8, 10 } },
  { "Dorian",            7, { 0, 2, 3, 5, 7, 9, 10 } },
  { "Mixolydian",        7, { 0, 2, 4, 5, 7, 9, 10 } },
  { "Blues",             6, { 0, 3, 5, 6, 7, 10 } },
};
static const uint8_t NUM_SCALES = sizeof(SCALES) / sizeof(SCALES[0]);

static const uint8_t ROOT_CC = 20, SCALE_CC = 21, CHORD_CC = 22;
static const uint8_t LATCH_CC = 23, RATE_CC = 24, SWING_CC = 25, TEMPO_CC = 26, ARP_CC = 27;
static const uint8_t SYNC_CC = 28;
static const uint8_t ARP_OCT_CC = 29;
static const uint8_t LOOP_CC = 30, LOOP_BARS_CC = 31;
static const uint8_t NUM_CHORDS = 3;                 // 0 single note, 1 triad, 2 seventh
static const uint8_t NUM_ARPS = 5;                   // 0 whole chord, 1 up, 2 down, 3 up-down, 4 random
static const uint16_t CLOCKS_PER_BAR = 96;           // 24 PPQN x 4 beats
static const uint16_t LOOP_EVENTS = 512;             // notes a take can hold
static const int EEPROM_ADDR = 64;                   // clear of SMCPadStock's settings at 0
static const uint32_t SETTINGS_MAGIC = 0x50444B07;   // "PDK" v7 (v6 had no arp octave range)
static const uint32_t SETTINGS_MAGIC_V6 = 0x50444B06;
static const uint32_t SETTINGS_MAGIC_V5 = 0x50444B05;
static const uint32_t SETTINGS_MAGIC_V4 = 0x50444B04;
static const uint32_t SETTINGS_MAGIC_V3 = 0x50444B03;
static const uint8_t DEFAULT_SCALE = 1;              // major pentatonic
static const uint8_t DEFAULT_CHORD = 2;              // the seventh is the cap
static const uint16_t DEFAULT_BPM = 120;

struct Settings {
  uint32_t magic;
  uint8_t root;                                      // 0..11 = C..B
  uint8_t scale;                                     // index into SCALES
  uint8_t chord;                                     // voice cap: 0 single, 1 triad, 2 seventh
  uint8_t latch;                                     // 0 momentary, 1 pads toggle
  uint8_t rate;                                      // 0 repeat off, 1..8 (RATE_CLOCKS)
  uint8_t swing;                                     // 0..4
  uint16_t bpm;                                      // 30..300
  uint8_t arp;                                       // 0 whole chord, 1..4 patterns
  uint8_t sync;                                      // 0 internal clock, 1 external MIDI clock
  uint8_t arpOct;                                    // 1..4 octaves the arp walk spans
};

static Settings settings;
static uint32_t settingsChangedAt;                   // 0: saved
static bool bleStarted;

struct Pad {
  bool held;                                         // finger down
  bool latched;                                      // toggled on by latch mode
  bool sounding;                                     // a note-on sequence is outstanding
  uint8_t soundingNotes[4];                          // exactly what was sent, so offs pair
  uint8_t soundingCount;
  uint8_t voice;                                     // voice the sounding chord was built with
  uint8_t velocity;                                  // last press, for latched repeats
  uint8_t aftertouch;
  uint32_t releasedAt;
  uint32_t flashUntil;                               // LED pulse after a repeat fire
  uint32_t gateOffAt;                                // micros; 0 = none
};

static Pad pad[16];

struct LoopEvent {
  uint16_t at;                                       // clock within the loop
  uint8_t status;                                    // 0x90 note on, 0x80 note off
  uint8_t note;
  uint8_t vel;
};

// note repeat clock: 24 clocks per quarter, one step per RATE_CLOCKS entry
static const uint8_t RATE_CLOCKS[8] = {24, 16, 12, 8, 6, 4, 3, 2};
static uint32_t clockCount;
static uint32_t nextClockUs;
static bool triggerPending;
static uint32_t triggerAt;
static uint32_t stepUs;
static uint32_t externalIntervalUs = 20833;          // measured, starts at 120 BPM
static uint32_t lastExternalUs;

// arpeggiator: the merged notes of every held/latched pad, over the octave range
static const uint8_t ARP_CAP = 128;                  // at most 22 merged notes x 4 octaves
static uint8_t arpNotes[ARP_CAP];                    // ascending, deduplicated
static uint16_t arpOwners[ARP_CAP];                  // bit per owner pad
static uint8_t arpCount;
static uint8_t arpIndex;                             // slot to play next
static uint8_t arpLast;                              // note played last, to re-anchor
static int8_t arpDir = 1;                            // up-down direction
static bool arpStarted;
static uint8_t arpSingle;                            // sounding arp note, 0 = none
static uint32_t arpGateOffAt;                        // micros; 0 = none
static uint32_t arpKey;                              // signature of the merged set
static uint32_t arpVoices;                           // 2 bits per pad: the voices behind it

// loop recorder: what was played, by clock position
enum { LOOP_OFF, LOOP_ARM_REC, LOOP_REC, LOOP_ARM_PLAY, LOOP_PLAY };
static uint8_t loopMode;
static uint8_t loopBars = 2;                         // 1..4, runtime only
static uint16_t loopPos;
static uint16_t loopCount;
static LoopEvent loopEvents[LOOP_EVENTS];
static uint32_t loopNoteBits[4];                     // notes the playback left sounding

static void settingsDefaults() {
  settings = Settings{SETTINGS_MAGIC, 0, DEFAULT_SCALE, DEFAULT_CHORD, 0, 0, 0, DEFAULT_BPM, 0, 0, 1};
}

static void settingsChanged() {
  settingsChangedAt = millis() | 1;
}

static void settingsLoad() {
  EEPROM.get(EEPROM_ADDR, settings);
  uint32_t magic = settings.magic;
  if (magic == SETTINGS_MAGIC_V6 || magic == SETTINGS_MAGIC_V5 || magic == SETTINGS_MAGIC_V4
      || magic == SETTINGS_MAGIC_V3) {
    bool v6 = magic == SETTINGS_MAGIC_V6;
    bool v5 = magic == SETTINGS_MAGIC_V5;
    bool v3 = magic == SETTINGS_MAGIC_V3;
    settings.magic = SETTINGS_MAGIC;
    settings.arpOct = 1;
    if (!v6) settings.sync = 0;
    if (!v6 && !v5) settings.arp = 0;
    if (v3) {                                        // v3 had no latch/repeat settings
      settings.latch = 0;
      settings.rate = 0;
      settings.swing = 0;
      settings.bpm = DEFAULT_BPM;
    }
    if (settings.root >= 12) settings.root = 0;
    if (settings.scale >= NUM_SCALES) settings.scale = DEFAULT_SCALE;
    if (settings.chord >= NUM_CHORDS) settings.chord = DEFAULT_CHORD;
    if (settings.latch > 1) settings.latch = 0;
    if (settings.rate > 8) settings.rate = 0;
    if (settings.swing > 4) settings.swing = 0;
    if (settings.bpm < 30 || settings.bpm > 300) settings.bpm = DEFAULT_BPM;
    if (settings.arp >= NUM_ARPS) settings.arp = 0;
    if (settings.arpOct < 1 || settings.arpOct > 4) settings.arpOct = 1;
    settingsChanged();                               // rewritten as v7 once idle
  } else if (settings.magic != SETTINGS_MAGIC) {
    settingsDefaults();
  } else if (settings.root >= 12 || settings.scale >= NUM_SCALES || settings.chord >= NUM_CHORDS
             || settings.latch > 1 || settings.rate > 8 || settings.swing > 4
             || settings.bpm < 30 || settings.bpm > 300 || settings.arp >= NUM_ARPS
             || settings.sync > 1 || settings.arpOct < 1 || settings.arpOct > 4) {
    settingsDefaults();
  }
}

// written 2 s after the last change, so turning a knob does not wear the flash
static void settingsSaveWhenIdle() {
  if (settingsChangedAt && millis() - settingsChangedAt > 2000) {
    EEPROM.put(EEPROM_ADDR, settings);
    EEPROM.commit();
    settingsChangedAt = 0;
  }
}

static int chordNotes(int i, uint8_t *out, uint8_t voice) {   // 1..4 notes, stacked in scale degrees
  const Scale &s = SCALES[settings.scale];
  int n = voice == 2 ? 4 : voice == 1 ? 3 : 1;
  for (int k = 0; k < n; k++) {
    int d = i + 2 * k;                                // the scale degree, octaves wrapping
    out[k] = BASE_NOTE + settings.root + 12 * (d / s.count) + s.step[d % s.count];
  }
  return n;
}

static int pressureLevel(int raw) {
  return constrain((raw - 150) * 127 / (960 - 150), 0, 127);
}

static uint8_t levelToVoice(int level) {
  return level < 43 ? 0 : level < 85 ? 1 : 2;
}

// a pad under the finger opens its chord by pressure; latched/struck pads hold the cap
static uint8_t effectiveVoice(int i) {
  if (!pad[i].held) return settings.chord;
  uint8_t v = levelToVoice(pressureLevel(SMCPad.padPressure(i)));
  return v > settings.chord ? settings.chord : v;
}

static uint32_t dim(uint32_t c, int num, int den) {
  uint32_t r = ((c >> 16) & 0xFF) * num / den, g = ((c >> 8) & 0xFF) * num / den, b = (c & 0xFF) * num / den;
  return r << 16 | g << 8 | b;
}

static uint32_t towardWhite(uint32_t c, int amount) {       // amount 0..127
  uint32_t r = ((c >> 16) & 0xFF), g = ((c >> 8) & 0xFF), b = (c & 0xFF);
  r += (255 - r) * amount / 127;
  g += (255 - g) * amount / 127;
  b += (255 - b) * amount / 127;
  return r << 16 | g << 8 | b;
}

static uint32_t wheel(uint8_t pos) {                 // 0..255 around the colour wheel
  pos = 255 - pos;
  if (pos < 85) return (uint32_t)(255 - pos * 3) << 16 | (uint32_t)(pos * 3);
  if (pos < 170) { pos -= 85; return (uint32_t)(pos * 3) << 8 | (uint32_t)(255 - pos * 3); }
  pos -= 170;
  return (uint32_t)(pos * 3) << 16 | (uint32_t)(255 - pos * 3) << 8;
}

static uint32_t padColor(int i) {
  return wheel(i * 255 / 16);
}

static void midi(uint8_t status, uint8_t d1, uint8_t d2) {
  uint8_t kind = status & 0xF0;
  if (loopMode == LOOP_REC && (kind == 0x90 || kind == 0x80) && loopCount < LOOP_EVENTS)
    loopEvents[loopCount++] = LoopEvent{loopPos, status, d1, d2};
  MidiUSB.sendMIDI({(uint8_t)(status >> 4), status, d1, d2});
  MidiUSB.flush();
  if (bleStarted && BLEMIDI.connected()) BLEMIDI.send(status, d1, d2);
}

static void silence(int i) {
  if (!pad[i].sounding) return;
  for (int k = 0; k < pad[i].soundingCount; k++) midi(0x80 | CH, pad[i].soundingNotes[k], 0);
  pad[i].sounding = false;
}

static void sound(int i, uint8_t velocity) {
  silence(i);
  uint8_t notes[4];
  pad[i].voice = effectiveVoice(i);
  pad[i].soundingCount = chordNotes(i, notes, pad[i].voice);
  for (int k = 0; k < pad[i].soundingCount; k++) {
    pad[i].soundingNotes[k] = notes[k];
    midi(0x90 | CH, notes[k], velocity);
  }
  pad[i].sounding = true;
}

// change the voice mid-hold: only the tail of the chord is added or dropped
static void setVoice(int i, uint8_t voice) {
  if (!pad[i].sounding) return;
  uint8_t notes[4];
  int n = chordNotes(i, notes, voice);
  int old = pad[i].soundingCount;
  uint8_t vel = pad[i].held ? constrain(pressureLevel(SMCPad.padPressure(i)), 1, 127)
                            : pad[i].velocity;
  for (int k = old; k < n; k++) {
    pad[i].soundingNotes[k] = notes[k];
    midi(0x90 | CH, notes[k], vel);
  }
  for (int k = n; k < old; k++) midi(0x80 | CH, pad[i].soundingNotes[k], 0);
  pad[i].soundingCount = n;
  pad[i].voice = voice;
  if (n > old) pad[i].aftertouch = 0;                // re-send 0xA0 to the new notes
}

static void arpSilence() {
  if (arpSingle) {
    midi(0x80 | CH, arpSingle, 0);
    arpSingle = 0;
    arpGateOffAt = 0;
  }
}

// the notes of every held/latched pad, merged, deduplicated and sorted,
// repeated up through the octave range
static bool arpRefresh() {
  uint16_t owners = 0;
  uint32_t voices = 0;
  arpCount = 0;
  for (int i = 0; i < 16; i++) {
    if (!(pad[i].held || pad[i].latched)) continue;
    owners |= 1 << i;
    voices |= (uint32_t)effectiveVoice(i) << (2 * i);
  }
  for (int o = 0; o < settings.arpOct; o++) {
    for (int i = 0; i < 16; i++) {
      if (!(owners & (1 << i))) continue;
      uint8_t notes[4];
      int n = chordNotes(i, notes, effectiveVoice(i));
      for (int k = 0; k < n; k++) {
        uint8_t note = notes[k] + 12 * o;
        int slot = 0;
        while (slot < arpCount && arpNotes[slot] < note) slot++;
        if (slot < arpCount && arpNotes[slot] == note) {
          arpOwners[slot] |= 1 << i;                 // reachable from two octaves
        } else if (arpCount < ARP_CAP) {
          for (int m = arpCount; m > slot; m--) {
            arpNotes[m] = arpNotes[m - 1];
            arpOwners[m] = arpOwners[m - 1];
          }
          arpNotes[slot] = note;
          arpOwners[slot] = 1 << i;
          arpCount++;
        }
      }
    }
  }
  uint32_t key = owners | (uint32_t)settings.root << 16 | (uint32_t)settings.scale << 20
                 | (uint32_t)settings.chord << 24 | (uint32_t)settings.arpOct << 26;
  bool changed = key != arpKey || voices != arpVoices;
  arpKey = key;
  arpVoices = voices;
  return changed;
}

static void arpFire() {
  bool changed = arpRefresh();
  if (!arpCount) {
    arpSilence();
    arpStarted = false;
    arpLast = 0;
    return;
  }
  if (settings.arp == 4) {
    arpIndex = random(arpCount);
  } else {
    bool advance = arpStarted;
    if (!arpStarted) {
      arpStarted = true;
      arpDir = 1;
      arpIndex = settings.arp == 2 ? arpCount - 1 : 0;
      advance = false;
    } else if (changed) {                            // re-anchor on the note being played
      int slot = -1;
      for (int i = 0; i < arpCount; i++) if (arpNotes[i] == arpLast) { slot = i; break; }
      advance = slot >= 0;
      arpIndex = slot >= 0 ? slot : settings.arp == 2 ? arpCount - 1 : 0;
    }
    if (advance) {
      if (settings.arp == 1) arpIndex = (arpIndex + 1) % arpCount;
      else if (settings.arp == 2) arpIndex = arpIndex ? arpIndex - 1 : arpCount - 1;
      else if (arpCount > 1) {                       // up-down: turn around without repeating
        int next = arpIndex + arpDir;
        if (next >= arpCount) { arpDir = -1; arpIndex = arpCount - 2; }
        else if (next < 0) { arpDir = 1; arpIndex = 1; }
        else arpIndex = next;
      }
    }
  }
  uint8_t note = arpNotes[arpIndex];
  uint16_t owners = arpOwners[arpIndex];
  int owner = 0;
  while (!(owners & (1 << owner))) owner++;
  uint8_t velocity = pad[owner].held ? constrain(pressureLevel(SMCPad.padPressure(owner)), 1, 127)
                                     : pad[owner].velocity;
  arpSilence();
  midi(0x90 | CH, note, velocity);
  arpSingle = note;
  arpGateOffAt = (micros() + stepUs / 2) | 1;
  for (int i = 0; i < 16; i++)
    if (owners & (1 << i)) pad[i].flashUntil = millis() + 40;
  arpLast = note;
}

static void setRoot(uint8_t root) {
  if (root != settings.root) {
    settings.root = root;
    settingsChanged();
  }
}

static void setScale(uint8_t scale) {
  if (scale != settings.scale) {
    settings.scale = scale;
    settingsChanged();
  }
}

static void setChord(uint8_t chord) {
  if (chord == settings.chord) return;
  settings.chord = chord;
  if (!settings.rate)                                // re-voice what is already sounding
    for (int i = 0; i < 16; i++)
      if (pad[i].sounding) setVoice(i, effectiveVoice(i));
  settingsChanged();
}

static void setSwing(uint8_t swing) {
  if (swing > 4) swing = 4;
  if (swing != settings.swing) {
    settings.swing = swing;
    settingsChanged();
  }
}

static void setBpm(uint16_t bpm) {
  bpm = constrain(bpm, 30, 300);
  if (bpm != settings.bpm) {
    settings.bpm = bpm;
    settingsChanged();
  }
}

static void setArp(uint8_t arp) {
  if (arp == settings.arp) return;
  settings.arp = arp;
  triggerPending = false;
  arpSilence();
  arpStarted = false;
  arpLast = 0;
  arpDir = 1;
  arpKey = 0;
  if (settings.rate) {                               // drop any chord a repeat left sounding
    for (int i = 0; i < 16; i++) {
      pad[i].gateOffAt = 0;
      silence(i);
    }
  }
  settingsChanged();
}

static void setArpOct(uint8_t oct) {
  oct = constrain(oct, 1, 4);
  if (oct == settings.arpOct) return;
  settings.arpOct = oct;
  settingsChanged();
}

static void setLatch(uint8_t latch) {
  if (latch == settings.latch) return;
  settings.latch = latch;
  if (!latch) {                                      // let go of every latched pad
    bool any = false;
    for (int i = 0; i < 16; i++) {
      if (pad[i].latched) {
        pad[i].latched = false;
        silence(i);
        pad[i].releasedAt = millis();
      }
      if (pad[i].held) any = true;
    }
    if (!any) {
      arpSilence();
      arpStarted = false;
      arpLast = 0;
    }
  }
  settingsChanged();
}

static void setRate(uint8_t rate) {
  if (rate == settings.rate) return;
  settings.rate = rate;
  triggerPending = false;
  if (!rate) {                                       // stop the gates, keep the sound
    arpSilence();
    arpStarted = false;
    arpLast = 0;
    arpDir = 1;
    for (int i = 0; i < 16; i++) {
      pad[i].gateOffAt = 0;
      if (pad[i].latched) {
        pad[i].latched = false;
        silence(i);
        pad[i].releasedAt = millis();
      } else if (pad[i].held && !pad[i].sounding) {
        sound(i, constrain(pressureLevel(SMCPad.padPressure(i)), 1, 127));
      }
    }
  }
  settingsChanged();
}

static uint16_t loopLengthClocks() {
  return (uint16_t)loopBars * CLOCKS_PER_BAR;
}

static void loopOffs() {                             // silence whatever the playback left on
  for (int n = 0; n < 128; n++)
    if (loopNoteBits[n >> 5] & (1u << (n & 31))) midi(0x80 | CH, (uint8_t)n, 0);
  for (int w = 0; w < 4; w++) loopNoteBits[w] = 0;
}

static void loopStop() {
  loopOffs();
  loopMode = LOOP_OFF;
}

static void setLoopBars(uint8_t bars) {
  bars = constrain(bars, 1, 4);
  if (bars == loopBars) return;
  loopStop();
  loopBars = bars;
}

static void setLoop(uint8_t value) {
  if (value == 0) {
    if (loopMode == LOOP_REC) loopCount = 0;         // an aborted take is discarded
    loopStop();
  } else if (value == 1) {
    if (!loopCount) return;
    loopStop();
    loopMode = LOOP_ARM_PLAY;
  } else if (value == 2) {
    loopStop();
    loopCount = 0;
    loopMode = LOOP_ARM_REC;
  } else if (value == 3) {
    loopStop();
    loopCount = 0;
  }
}

static void loopFire() {
  for (uint16_t i = 0; i < loopCount; i++) {
    if (loopEvents[i].at != loopPos) continue;
    uint8_t note = loopEvents[i].note;
    midi(loopEvents[i].status, note, loopEvents[i].vel);
    if ((loopEvents[i].status & 0xF0) == 0x90) loopNoteBits[note >> 5] |= 1u << (note & 31);
    else loopNoteBits[note >> 5] &= ~(1u << (note & 31));
  }
}

static void loopTick() {
  switch (loopMode) {
    case LOOP_ARM_REC:                               // this tick is the downbeat
      loopPos = 0;
      loopMode = LOOP_REC;
      break;
    case LOOP_REC:
      if (++loopPos >= loopLengthClocks()) {
        loopPos = 0;
        loopMode = LOOP_PLAY;
        loopFire();
      }
      break;
    case LOOP_ARM_PLAY:
      loopPos = 0;
      loopMode = LOOP_PLAY;
      loopFire();
      break;
    case LOOP_PLAY:
      if (++loopPos >= loopLengthClocks()) loopPos = 0;
      loopFire();
      break;
  }
}

static void clockTick();

static void setSync(uint8_t sync) {
  if (sync == settings.sync) return;
  settings.sync = sync;
  clockCount = 0;
  triggerPending = false;
  settingsChanged();
}

static void midiIn() {                               // the next CC 20..26 from USB, else BLE
  for (int n = 0; n < 32; n++) {
    midiEventPacket_t p = MidiUSB.read();
    if (!p.header && bleStarted) p = BLEMIDI.read();
    if (!p.header) return;
    if (p.byte1 == 0xF8) {
      if (settings.sync) {                           // the host's 24-PPQN clock
        uint32_t now = micros();
        uint32_t dt = now - lastExternalUs;
        if (dt < 200000) externalIntervalUs = (externalIntervalUs * 3 + dt) / 4;
        lastExternalUs = now;
        clockTick();
      }
    } else if (p.byte1 == 0xFA) {                    // start: the grid restarts here
      clockCount = 0;
      triggerPending = false;
    } else if (p.byte1 == 0xFC) {                    // stop: the internal clock takes over
      lastExternalUs = micros() - 500000;
    } else if ((p.byte1 & 0xF0) == 0xB0) {
      if (p.byte2 == ROOT_CC) setRoot(p.byte3 % 12);
      else if (p.byte2 == SCALE_CC) setScale(p.byte3 % NUM_SCALES);
      else if (p.byte2 == CHORD_CC) setChord(p.byte3 % NUM_CHORDS);
      else if (p.byte2 == LATCH_CC) setLatch(p.byte3 ? 1 : 0);
      else if (p.byte2 == RATE_CC) setRate(p.byte3 > 8 ? 8 : p.byte3);
      else if (p.byte2 == SWING_CC) setSwing(p.byte3);
      else if (p.byte2 == TEMPO_CC) setBpm((uint16_t)(30 + p.byte3 * 270 / 127));
      else if (p.byte2 == ARP_CC) setArp(p.byte3 > NUM_ARPS - 1 ? NUM_ARPS - 1 : p.byte3);
      else if (p.byte2 == SYNC_CC) setSync(p.byte3 ? 1 : 0);
      else if (p.byte2 == ARP_OCT_CC) setArpOct(p.byte3);
      else if (p.byte2 == LOOP_CC) setLoop(p.byte3);
      else if (p.byte2 == LOOP_BARS_CC) setLoopBars(p.byte3);
    }
  }
}

static bool externalAlive() {
  return settings.sync && micros() - lastExternalUs < 500000;
}

static uint32_t clockIntervalUs() {
  return externalAlive() ? externalIntervalUs : 2500000UL / settings.bpm;
}

static void repeatFire() {
  for (int i = 0; i < 16; i++) {
    if (!(pad[i].held || pad[i].latched)) continue;
    uint8_t velocity = pad[i].held ? constrain(pressureLevel(SMCPad.padPressure(i)), 1, 127)
                                   : pad[i].velocity;
    sound(i, velocity);
    pad[i].gateOffAt = (micros() + stepUs / 2) | 1;
    pad[i].flashUntil = millis() + 40;
  }
}

static void clockTick() {
  clockCount++;
  loopTick();
  if (!settings.rate) return;
  uint8_t grid = RATE_CLOCKS[settings.rate - 1];
  if (clockCount % grid) return;
  stepUs = grid * clockIntervalUs();
  // swing: every second step later, by 0 / 8 / 16 / 24 / 32 % of a step (50..66 %)
  uint32_t late = ((clockCount / grid) & 1) ? stepUs * settings.swing * 8 / 100 : 0;
  triggerAt = micros() + late;
  triggerPending = true;
}

static void clockRun() {
  uint32_t now = micros();
  if (!externalAlive()) {                            // the host's clock drives clockTick()
    if ((int32_t)(now - nextClockUs) > 100000) nextClockUs = now;   // fell behind: restart
    while ((int32_t)(now - nextClockUs) >= 0) {
      clockTick();
      nextClockUs += clockIntervalUs();
    }
  }
  if (triggerPending && (int32_t)(now - triggerAt) >= 0) {
    triggerPending = false;
    if (settings.arp) arpFire();
    else repeatFire();
  }
  if (arpSingle && (int32_t)(now - arpGateOffAt) >= 0) arpSilence();
  for (int i = 0; i < 16; i++) {
    if (pad[i].gateOffAt && (int32_t)(now - pad[i].gateOffAt) >= 0) {
      pad[i].gateOffAt = 0;
      silence(i);
    }
  }
}

static void onPadPress(int i, int velocity) {
  uint8_t vel = constrain(velocity, 1, 127);
  pad[i].velocity = vel;
  pad[i].aftertouch = 0;
  if (settings.latch) {
    if (pad[i].latched) {
      pad[i].latched = false;
      silence(i);
      pad[i].releasedAt = millis();
    } else {
      pad[i].latched = true;
      if (!settings.rate) sound(i, vel);
    }
    return;
  }
  pad[i].held = true;
  if (!settings.rate) sound(i, vel);
}

static void onPadRelease(int i) {
  if (settings.latch) return;                        // the tap already toggled it
  pad[i].held = false;
  pad[i].releasedAt = millis();
  if (!settings.rate) silence(i);                    // else the pending gate-off does it
}

void setup() {
  settingsLoad();
  SMCPad.onPadPress(onPadPress);
  SMCPad.onPadRelease(onPadRelease);
  randomSeed(micros());                              // the random arp pattern
  bleStarted = BLEMIDI.begin("SMC-PAD Pocket");
}

static uint32_t loopColor(int slot, uint32_t now) {
  bool blink = (now / 250) & 1;
  if (loopMode == LOOP_ARM_REC) return blink ? 0x800000 : 0;
  if (loopMode == LOOP_REC) {
    int lit = (int)loopPos * 4 / loopLengthClocks();
    return slot < lit ? 0x800000 : 0x200000;
  }
  if (loopMode == LOOP_ARM_PLAY) return blink ? 0x008000 : 0;
  int beat = (loopPos % CLOCKS_PER_BAR) / 24;        // 0..3 within the bar
  return slot == beat ? 0x40FF40 : 0x002000;
}

void loop() {
  SMCPad.update();
  midiIn();
  settingsSaveWhenIdle();
  if (settings.rate || loopMode) clockRun();
  uint32_t now = millis();
  const Scale &s = SCALES[settings.scale];
  for (int i = 0; i < 16; i++) {
    if (pad[i].held || pad[i].latched) {
      int level = pad[i].held ? pressureLevel(SMCPad.padPressure(i)) : pad[i].velocity;
      if (pad[i].held && !settings.rate && pad[i].sounding) {
        uint8_t voice = effectiveVoice(i);
        if (voice != pad[i].voice) setVoice(i, voice);
      }
      if (pad[i].held && !settings.rate && pad[i].sounding
          && abs(level - pad[i].aftertouch) >= 3) {
        pad[i].aftertouch = level;
        for (int k = 0; k < pad[i].soundingCount; k++) midi(0xA0 | CH, pad[i].soundingNotes[k], level);
      }
      uint32_t c = dim(padColor(i), 70 + level * 130 / 127, 255);
      c = towardWhite(c, level * 48 / 127);
      if (now < pad[i].flashUntil) c = towardWhite(c, 90);
      SMCPad.padColor(i, c);
    } else {
      uint32_t age = now - pad[i].releasedAt;
      if (pad[i].velocity && age < 400) {
        int bright = (255 - (int)(age * 255 / 400)) * pad[i].velocity / 127;
        SMCPad.padColor(i, dim(padColor(i), bright, 255));
      } else if (loopMode && i >= 12) {
        SMCPad.padColor(i, loopColor(i - 12, now));  // the top row shows the loop
      } else if (i % s.count == 0) {
        SMCPad.padColor(i, towardWhite(dim(padColor(i), 90, 255), 12));
      } else {
        SMCPad.padColor(i, dim(padColor(i), 45, 255));
      }
    }
  }
  SMCPad.show();
}
