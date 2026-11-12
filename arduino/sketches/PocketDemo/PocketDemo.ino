// The Pocket on its own: 16 pads played by hand, in a selectable key.
//   - the pads walk the selected scale: pad 1 is the root, the next pads the next
//     scale notes, on up through the octaves
//   - each pad plays the chord of its scale degree: a triad (the default) or a seventh,
//     stacked from the scale's degrees; CC 22 picks the voice (0 single, 1 triad, 2 seventh)
//   - MIDI CC 20 picks the root (0..11 = C..B), CC 21 the scale (0 chromatic,
//     1 major pentatonic, 2 minor pentatonic, 3 major, 4 natural minor, 5 dorian,
//     6 mixolydian, 7 blues), on any channel, from USB or BLE; the choices are kept in flash
//   - each pad sends Note On/Off on channel 1, velocity from how hard it is hit
//   - holding a pad sends polyphonic aftertouch (0xA0) for every note of its chord,
//     following the pressure
//   - everything goes out over USB-MIDI and, when a host listens, BLE-MIDI (untethered)
//   - the pad LED is the pad's colour; root pads sit brighter towards white, pressure
//     brightens a pad towards white, and a released pad fades out like the notes it played
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
static const uint8_t NUM_CHORDS = 3;                 // 0 single note, 1 triad, 2 seventh
static const int EEPROM_ADDR = 64;                   // clear of SMCPadStock's settings at 0
static const uint32_t SETTINGS_MAGIC = 0x50444B03;   // "PDK" v3 (v2 had no chord byte)
static const uint8_t DEFAULT_SCALE = 1;              // major pentatonic
static const uint8_t DEFAULT_CHORD = 1;              // triad

struct Settings {
  uint32_t magic;
  uint8_t root;                                      // 0..11 = C..B
  uint8_t scale;                                     // index into SCALES
  uint8_t chord;                                     // 0 single note, 1 triad, 2 seventh
};

static Settings settings;
static uint32_t settingsChangedAt;                   // 0: saved
static bool bleStarted;

struct Pad {
  bool held;
  uint8_t notes[4];                                  // the chord: 1..4 notes
  uint8_t count;
  uint8_t velocity;
  uint8_t aftertouch;
  uint32_t releasedAt;
};

static Pad pad[16];

static void settingsDefaults() {
  settings = Settings{SETTINGS_MAGIC, 0, DEFAULT_SCALE, DEFAULT_CHORD};
}

static void settingsLoad() {
  EEPROM.get(EEPROM_ADDR, settings);
  if (settings.magic != SETTINGS_MAGIC) settingsDefaults();   // v3 block: the v2 one had no chord
}

static void settingsChanged() {
  settingsChangedAt = millis() | 1;
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

static void midiIn() {                               // the next CC 20/21/22 from USB, else BLE
  for (int n = 0; n < 32; n++) {
    midiEventPacket_t p = MidiUSB.read();
    if (!p.header && bleStarted) p = BLEMIDI.read();
    if (!p.header) return;
    if ((p.byte1 & 0xF0) == 0xB0) {
      if (p.byte2 == ROOT_CC) setRoot(p.byte3 % 12);
      else if (p.byte2 == SCALE_CC) setScale(p.byte3 % NUM_SCALES);
      else if (p.byte2 == CHORD_CC) setChord(p.byte3 % NUM_CHORDS);
    }
  }
}

static void onPadPress(int i, int velocity) {
  pad[i].held = true;
  pad[i].count = chordNotes(i, pad[i].notes);
  pad[i].velocity = constrain(velocity, 1, 127);
  pad[i].aftertouch = 0;
  for (int k = 0; k < pad[i].count; k++) midi(0x90 | CH, pad[i].notes[k], pad[i].velocity);
}

static void onPadRelease(int i) {
  pad[i].held = false;
  pad[i].releasedAt = millis();
  for (int k = 0; k < pad[i].count; k++) midi(0x80 | CH, pad[i].notes[k], 0);
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
  uint32_t now = millis();
  const Scale &s = SCALES[settings.scale];
  for (int i = 0; i < 16; i++) {
    if (pad[i].held) {
      int level = pressureLevel(SMCPad.padPressure(i));
      if (abs(level - pad[i].aftertouch) >= 3) {
        pad[i].aftertouch = level;
        for (int k = 0; k < pad[i].count; k++) midi(0xA0 | CH, pad[i].notes[k], level);
      }
      uint32_t c = dim(padColor(i), 70 + level * 130 / 127, 255);
      SMCPad.padColor(i, towardWhite(c, level * 48 / 127));
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
