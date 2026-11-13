#include "keyer.h"

#include "bridge_config.h"
#include "link.h"
#include "state.h"
#include "status.h"
#include "usb_host.h"

static uint8_t s_phase = KEYER_IDLE;
static uint32_t s_start = 0, s_keys = 0, s_lowSince = 0, s_lastBlink = 0, s_lastSt = 0;
static bool s_led = false;

static void emitKeyer() {
  KeyerSt st = {};
  st.phase = s_phase;
  st.keys = s_keys;
  st.ms = s_start ? millis() - s_start : 0;
  linkSend(MSG_KEYER_ST, &st, sizeof(st));
}

static void setPhase(uint8_t phase) {
  s_phase = phase;
  g_state.keyer = phase;
  g_state.dirty = true;
  emitKeyer();
}

static void parkLow(uint8_t pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}

static void releaseBus() {
  pinMode(PIN_KEY_DP, INPUT);
  pinMode(PIN_KEY_DM, INPUT);
}

static void driveBus() {
  digitalWrite(PIN_KEY_DP, LOW);
  digitalWrite(PIN_KEY_DM, LOW);
  pinMode(PIN_KEY_DP, OUTPUT);
  pinMode(PIN_KEY_DM, OUTPUT);
}

static bool lineHighPulledDown(uint8_t pin, uint32_t settle_us) {
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(settle_us);
  bool up = digitalRead(pin);
  pinMode(pin, INPUT);
  return up;
}

// D+ is only released while the clock is low, so seeing it high means the chip took the key
// and enabled its 1.5k pull-up. Sense with the internal pull-down: a floating line otherwise
// keeps its last level in the cable's capacitance.
static bool chipPulledUp() {
  bool up = lineHighPulledDown(PIN_KEY_DP, SENSE_US);
  if (!up) parkLow(PIN_KEY_DP);
  return up;
}

// After the ACK the chip needs 1 ms falling edges on D+ to calibrate its oscillator. The host
// stack is not running yet, so synthesize them until it drops its pull-up.
static bool clockTrain() {
  for (uint32_t i = 0; i < TRAIN_PULSES; i++) {
    if (!lineHighPulledDown(PIN_KEY_DP, SENSE_US)) return true;
    digitalWrite(PIN_KEY_DP, LOW);
    pinMode(PIN_KEY_DP, OUTPUT);
    delayMicroseconds(10);
    pinMode(PIN_KEY_DP, INPUT);
    delayMicroseconds(1000 - 10 - SENSE_US);
  }
  return false;
}

// 1 = the chip took the bus, -1 = a pull-up that never dropped (a firmware is running),
// 0 = no acknowledgement yet.
static int sendKeyOnce() {
  noInterrupts();
  for (int bit = 15; bit >= 0; bit--) {
    if (chipPulledUp()) {
      interrupts();
      bool ok = clockTrain();
      releaseBus();
      return ok ? 1 : -1;
    }
    digitalWrite(PIN_KEY_DM, (USB_KEY >> bit) & 1);
    delayMicroseconds(HALF_PERIOD_US - SENSE_US);
    digitalWrite(PIN_KEY_DP, HIGH);
    delayMicroseconds(HALF_PERIOD_US);
    digitalWrite(PIN_KEY_DP, LOW);
  }
  interrupts();
  return 0;
}

// D+ high before the chip was ever seen low is a running firmware, or one that dropped off
// USB but left its pull-up on. Arm only on a truly detached bus.
static bool dpReleasedHigh() {
  pinMode(PIN_KEY_DM, INPUT);
  return lineHighPulledDown(PIN_KEY_DP, PROBE_US);
}

void keyerInit() {
  releaseBus();
  statusRGB(0, 0, 0);
}

void keyerHandleCommand(const KeyerCmd &cmd) {
  if (g_state.mode != MODE_UBOOT) {
    linkLog("keyer: switch to uboot mode first");
    return;
  }
  switch (cmd.op) {
    case KEYER_OP_ARM:
      releaseBus();
      s_start = millis();
      s_lowSince = s_start;
      s_lastSt = s_start;
      s_keys = 0;
      setPhase(KEYER_ARMED);
      statusRGB(0, 0, 0);
      linkLog("keyer armed: power on the pad now");
      break;
    case KEYER_OP_DISARM:
      releaseBus();
      setPhase(KEYER_IDLE);
      statusRGB(0, 0, 0);
      linkLog("keyer disarmed");
      break;
    case KEYER_OP_REPLUG:
      driveBus();
      delay(300);
      releaseBus();
      linkLog("replug: SE0 300 ms");
      break;
    case KEYER_OP_HOST:
      usbHostBegin();
      break;
    default:
      break;
  }
}

void keyerTick() {
  switch (s_phase) {
    case KEYER_ARMED:
      if (dpReleasedHigh()) {
        s_lowSince = millis();
      } else if (millis() - s_lowSince >= ARM_LOW_MS) {
        driveBus();
        setPhase(KEYER_KEYING);
        linkLog("D+ low: keying");
      }
      break;

    case KEYER_KEYING: {
      int r = sendKeyOnce();
      if (r == 1) {
        releaseBus();
        setPhase(KEYER_OK);
        statusRGB(0, 60, 0);
        linkLog("key accepted after %lu keys; starting host", (unsigned long)s_keys);
        usbHostBegin();
      } else if (r < 0) {
        releaseBus();
        setPhase(KEYER_BLOCKED);
        statusRGB(60, 0, 0);
        linkLog("D+ pulled up but never dropped: a firmware is running");
      } else {
        s_keys++;
        if (millis() - s_lastSt >= 500) {
          s_lastSt = millis();
          emitKeyer();
        }
        if (millis() - s_start > TIMEOUT_MS) {
          releaseBus();
          setPhase(KEYER_TIMEOUT);
          statusRGB(60, 0, 0);
          linkLog("timeout: no response, check the wiring");
        } else if (millis() - s_lastBlink > 50) {
          s_led = !s_led;
          statusRGB(0, 0, s_led ? 60 : 0);
          s_lastBlink = millis();
        }
      }
      break;
    }

    default:
      break;
  }
}
