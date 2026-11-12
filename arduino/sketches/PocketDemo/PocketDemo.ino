// The Pocket on its own: 16 pads played by hand, in a selectable key.
//   - the pads walk the selected scale: pad 1 is the root, the next pads the next
//     scale notes, on up through the octaves
//   - each pad plays the chord of its scale degree: a triad (the default) or a seventh,
//     stacked from the scale's degrees; CC 22 picks the voice (0 single, 1 triad, 2 seventh)
//   - MIDI CC 20 picks the root (0..11 = C..B), CC 21 the scale (0 chromatic,
//     1 major pentatonic, 2 minor pentatonic, 3 major, 4 natural minor, 5 dorian,
//     6 mixolydian, 7 blues), on any channel, from USB or BLE; the choices are kept in flash
//   - latch (CC 23: 0 off, 1 on): a tap toggles a pad on or off instead of momentary
//   - note repeat (CC 24: 0 off, 1..8 = 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T):
//     held and latched pads retrigger from the internal clock, with a 50% gate; CC 25 picks
//     the swing (0..4 = 50..66%) and CC 26 the tempo (0..127 = 30..300 BPM)
//   - each pad sends Note On/Off on channel 1, velocity from how hard it is hit
//   - holding a pad sends polyphonic aftertouch (0xA0) for every note of its chord,
//     following the pressure (repeating pads are articulated by their retriggers instead)
//   - everything goes out over USB-MIDI and, when a host listens, BLE-MIDI (untethered)
//   - the pad LED is the pad's colour; root pads sit brighter towards white, pressure
//     brightens a pad towards white, a repeat fire flashes it, and a released pad fades out
//     like the notes it played
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
static const uint8_t LATCH_CC = 23, RATE_CC = 24, SWING_CC = 25, TEMPO_CC = 26;
static const uint8_t NUM_CHORDS = 3;                 // 0 single note, 1 triad, 2 seventh
static const int EEPROM_ADDR = 64;                   // clear of SMCPadStock's settings at 0
static const uint32_t SETTINGS_MAGIC = 0x50444B04;   // "PDK" v4 (v3 had no repeat settings)
static const uint32_t SETTINGS_MAGIC_V3 = 0x50444B03;
static const uint8_t DEFAULT_SCALE = 1;              // major pentatonic
static const uint8_t DEFAULT_CHORD = 1;              // triad
static const uint16_t DEFAULT_BPM = 120;

struct Settings {
  uint32_t magic;
  uint8_t root;                                      // 0..11 = C..B
  uint8_t scale;                                     // index into SCALES
  uint8_t chord;                                     // 0 single note, 1 triad, 2 seventh
  uint8_t latch;                                     // 0 momentary, 1 pads toggle
  uint8_t rate;                                      // 0 repeat off, 1..8 (RATE_CLOCKS)
  uint8_t swing;                                     // 0..4
  uint16_t bpm;                                      // 30..300
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
  uint8_t velocity;                                  // last press, for latched repeats
  uint8_t aftertouch;
  uint32_t releasedAt;
  uint32_t flashUntil;                               // LED pulse after a repeat fire
  uint32_t gateOffAt;                                // micros; 0 = none
};

static Pad pad[16];

// note repeat clock: 24 clocks per quarter, one step per RATE_CLOCKS entry
static const uint8_t RATE_CLOCKS[8] = {24, 16, 12, 8, 6, 4, 3, 2};
static uint32_t clockCount;
static uint32_t nextClockUs;
static bool triggerPending;
static uint32_t triggerAt;
static uint32_t stepUs;

static void settingsDefaults() {
  settings = Settings{SETTINGS_MAGIC, 0, DEFAULT_SCALE, DEFAULT_CHORD, 0, 0, 0, DEFAULT_BPM};
}

static void settingsChanged() {
  settingsChangedAt = millis() | 1;
}

static void settingsLoad() {
  EEPROM.get(EEPROM_ADDR, settings);
  if (settings.magic == SETTINGS_MAGIC_V3) {         // v3 block: keep the key and voice
    settings.magic = SETTINGS_MAGIC;
    settings.latch = 0;
    settings.rate = 0;
    settings.swing = 0;
    settings.bpm = DEFAULT_BPM;
    if (settings.root >= 12) settings.root = 0;
    if (settings.scale >= NUM_SCALES) settings.scale = DEFAULT_SCALE;
    if (settings.chord >= NUM_CHORDS) settings.chord = DEFAULT_CHORD;
    settingsChanged();                               // rewritten as v4 once idle
  } else if (settings.magic != SETTINGS_MAGIC) {
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

static int chordNotes(int i, uint8_t *out) {          // 1..4 notes, stacked in scale degrees
  const Scale &s = SCALES[settings.scale];
  int n = settings.chord == 2 ? 4 : settings.chord == 1 ? 3 : 1;
  for (int k = 0; k < n; k++) {
    int d = i + 2 * k;                                // the scale degree, octaves wrapping
    out[k] = BASE_NOTE + settings.root + 12 * (d / s.count) + s.step[d % s.count];
  }
  return n;
}

static int pressureLevel(int raw) {
  return constrain((raw - 150) * 127 / (960 - 150), 0, 127);
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
  pad[i].soundingCount = chordNotes(i, notes);
  for (int k = 0; k < pad[i].soundingCount; k++) {
    pad[i].soundingNotes[k] = notes[k];
    midi(0x90 | CH, notes[k], velocity);
  }
  pad[i].sounding = true;
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
  if (chord != settings.chord) {
    settings.chord = chord;
    settingsChanged();
  }
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

static void setLatch(uint8_t latch) {
  if (latch == settings.latch) return;
  settings.latch = latch;
  if (!latch) {                                      // let go of every latched pad
    for (int i = 0; i < 16; i++) {
      if (pad[i].latched) {
        pad[i].latched = false;
        silence(i);
        pad[i].releasedAt = millis();
      }
    }
  }
  settingsChanged();
}

static void setRate(uint8_t rate) {
  if (rate == settings.rate) return;
  settings.rate = rate;
  triggerPending = false;
  if (!rate) {                                       // stop the gates, keep the sound
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

static void midiIn() {                               // the next CC 20..26 from USB, else BLE
  for (int n = 0; n < 32; n++) {
    midiEventPacket_t p = MidiUSB.read();
    if (!p.header && bleStarted) p = BLEMIDI.read();
    if (!p.header) return;
    if ((p.byte1 & 0xF0) == 0xB0) {
      if (p.byte2 == ROOT_CC) setRoot(p.byte3 % 12);
      else if (p.byte2 == SCALE_CC) setScale(p.byte3 % NUM_SCALES);
      else if (p.byte2 == CHORD_CC) setChord(p.byte3 % NUM_CHORDS);
      else if (p.byte2 == LATCH_CC) setLatch(p.byte3 ? 1 : 0);
      else if (p.byte2 == RATE_CC) setRate(p.byte3 > 8 ? 8 : p.byte3);
      else if (p.byte2 == SWING_CC) setSwing(p.byte3);
      else if (p.byte2 == TEMPO_CC) setBpm((uint16_t)(30 + p.byte3 * 270 / 127));
    }
  }
}

static uint32_t clockIntervalUs() {
  return 2500000UL / settings.bpm;                   // 24 clocks per quarter
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
  if ((int32_t)(now - nextClockUs) > 100000) nextClockUs = now;   // fell behind: restart
  while ((int32_t)(now - nextClockUs) >= 0) {
    clockTick();
    nextClockUs += clockIntervalUs();
  }
  if (triggerPending && (int32_t)(now - triggerAt) >= 0) {
    triggerPending = false;
    repeatFire();
  }
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
  bleStarted = BLEMIDI.begin("SMC-PAD Pocket");
}

void loop() {
  SMCPad.update();
  midiIn();
  settingsSaveWhenIdle();
  if (settings.rate) clockRun();
  uint32_t now = millis();
  const Scale &s = SCALES[settings.scale];
  for (int i = 0; i < 16; i++) {
    if (pad[i].held || pad[i].latched) {
      int level = pad[i].held ? pressureLevel(SMCPad.padPressure(i)) : pad[i].velocity;
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
      } else if (i % s.count == 0) {
        SMCPad.padColor(i, towardWhite(dim(padColor(i), 90, 255), 12));
      } else {
        SMCPad.padColor(i, dim(padColor(i), 45, 255));
      }
    }
  }
  SMCPad.show();
}
