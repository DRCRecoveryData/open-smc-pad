// The Pocket on its own: 16 pads, one note each, played by hand.
//   - each pad sends Note On/Off on channel 1 (48..63), velocity from how hard it is hit
//   - holding a pad sends polyphonic aftertouch (0xA0) that follows the pressure
//   - everything goes out over USB-MIDI and, when a host listens, BLE-MIDI (untethered)
//   - the pad LED is the pad's colour; pressure brightens it towards white, and a
//     released pad fades out like the note it played
#include <SMCPad.h>
#include <MIDIUSB.h>
#include <BLEMIDI.h>

static const uint8_t CH = 0;                         // MIDI channel 1
static const uint8_t BASE_NOTE = 48;                 // C3 on pad 1

static bool bleStarted;

struct Pad {
  bool held;
  uint8_t velocity;
  uint8_t aftertouch;
  uint32_t releasedAt;
};

static Pad pad[16];

static int pressureLevel(int raw) {
  return constrain((raw - 150) * 127 / (960 - 150), 0, 127);
}

static uint32_t scale(uint32_t c, int num, int den) {
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

static void onPadPress(int i, int velocity) {
  pad[i].held = true;
  pad[i].velocity = constrain(velocity, 1, 127);
  pad[i].aftertouch = 0;
  midi(0x90 | CH, BASE_NOTE + i, pad[i].velocity);
}

static void onPadRelease(int i) {
  pad[i].held = false;
  pad[i].releasedAt = millis();
  midi(0x80 | CH, BASE_NOTE + i, 0);
}

void setup() {
  SMCPad.onPadPress(onPadPress);
  SMCPad.onPadRelease(onPadRelease);
  bleStarted = BLEMIDI.begin("SMC-PAD Pocket");
}

void loop() {
  SMCPad.update();
  uint32_t now = millis();
  for (int i = 0; i < 16; i++) {
    if (pad[i].held) {
      int level = pressureLevel(SMCPad.padPressure(i));
      if (abs(level - pad[i].aftertouch) >= 3) {
        pad[i].aftertouch = level;
        midi(0xA0 | CH, BASE_NOTE + i, level);
      }
      uint32_t c = scale(padColor(i), 70 + level * 130 / 127, 255);
      SMCPad.padColor(i, towardWhite(c, level * 48 / 127));
    } else {
      uint32_t age = now - pad[i].releasedAt;
      int bright = pad[i].velocity && age < 400 ? (255 - (int)(age * 255 / 400)) * pad[i].velocity / 127 : 45;
      SMCPad.padColor(i, scale(padColor(i), bright, 255));
    }
  }
  SMCPad.show();
}
