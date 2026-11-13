// jl-usbkey-s3: forces a JieLi chip (SMC-PAD: AC6966C / BR25) into its MaskROM USB download
// mode ("UBOOT") by sending the USB_KEY on the USB data lines at power-up. ESP32-S3-DevKitC-1
// port of hw/jl-usbkey; the protocol and behaviour are identical.
// Ref: tools/jielie/isp/usb/usb-key.md
//
// Wiring (ESP32-S3-DevKitC-1):
//   GPIO4 ---- USB D+  (green)   key clock
//   GPIO5 ---- USB D-  (white)   key data
//   GND ------ USB GND (black)
// Direct wires, no series resistors. The SMC-PAD stays connected to the computer through the
// tapped cable.
//
// Console is the board's UART USB-C port (CP210x), 115200 8N1: Serial is UART0 on GPIO43/44,
// so compile with "USB CDC On Boot: Disabled" (the default for esp32:esp32:esp32s3) and leave
// the native USB port unused.
//
// Use: open the serial monitor on the UART port. Nothing is keyed at boot, so the pad can stay
// wired to the bus permanently; send 'r' to start an attempt, then power the SMC-PAD on. The
// on-board RGB LED (WS2812, GPIO48 on DevKitC-1 v1.0, GPIO38 on v1.1; both are driven, so
// either revision lights): blue blink = keying, solid green = the chip took the USB_KEY, red
// blink = timed out.
// 'x' aborts an attempt; 'd' replugs the pad as far as
// the host can tell; 't' checks with the pad off whether the data lines really reach the
// computer; 'k' pulses D+ high for 30 ms; 'p' probes both lines.

#include <Arduino.h>

#ifndef CONFIG_IDF_TARGET_ESP32S3
#error "build this sketch for the ESP32-S3 (esp32:esp32:esp32s3)"
#endif

constexpr uint8_t PIN_RGB_S3_V10 = 48;   // DevKitC-1 v1.0
constexpr uint8_t PIN_RGB_S3_V11 = 38;   // DevKitC-1 v1.1

constexpr uint8_t PIN_DP = 4;            // D+ : clock, data latched on rising edge
constexpr uint8_t PIN_DM = 5;            // D- : data, MSB first
constexpr uint16_t USB_KEY = 0x16EF;
constexpr uint32_t HALF_PERIOD_US = 10;  // ~50 kHz clock
constexpr uint32_t SENSE_US = 3;         // settle time before reading released D+
constexpr uint32_t PROBE_US = 300;       // pull-down settle that outlasts cable capacitance
constexpr uint32_t TIMEOUT_MS = 600000;
constexpr uint32_t ARM_LOW_MS = 10;      // D+ must stay low this long before keying
constexpr uint32_t TRAIN_PULSES = 150;   // worst-case calibration window, 1 ms per pulse

static void parkLow(uint8_t pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}

// pinMode() on this core configures the pad pulls too: INPUT disables both.
static void releaseBus() {
  pinMode(PIN_DP, INPUT);
  pinMode(PIN_DM, INPUT);
}

static void driveBus() {
  digitalWrite(PIN_DP, LOW);
  digitalWrite(PIN_DM, LOW);
  pinMode(PIN_DP, OUTPUT);
  pinMode(PIN_DM, OUTPUT);
}

// Status colour on the on-board WS2812 via the core's RMT driver. Both revision pins are
// driven: the one without an LED is a free header pin and only sees the same short pulse train.
static void statusRGB(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(PIN_RGB_S3_V10, r, g, b);
  rgbLedWrite(PIN_RGB_S3_V11, r, g, b);
}

static bool lineHighPulledDown(uint8_t pin, uint32_t settle_us) {
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(settle_us);
  bool up = digitalRead(pin);
  pinMode(pin, INPUT);
  return up;
}

// D+ is only ever released while the clock is low, so seeing it high means the chip has
// acknowledged the key and enabled its 1.5k pull-up for SOF calibration. Sense with the
// internal pull-down on: a released line with nothing on it otherwise keeps its last level in
// the pad's input capacitance and reads high for minutes.
static bool chipPulledUp() {
  bool up = lineHighPulledDown(PIN_DP, SENSE_US);
  if (!up) parkLow(PIN_DP);
  return up;
}

// After the acknowledge the chip pulls D+ up and measures the 1 ms frame clock from falling
// edges on that line before it brings its USB up. An idle host port sends no SOFs, so the chip
// locks onto noise and its PLL comes up wrong -- the host then never enumerates it. Give it the
// edges ourselves: 10 us low every 1 ms, until it drops its pull-up (calibration over).
// True only if the pull-up went away: a running firmware keeps it up, so that is no success.
static bool clockTrain() {
  for (uint32_t i = 0; i < TRAIN_PULSES; i++) {
    if (!lineHighPulledDown(PIN_DP, SENSE_US)) return true;   // pull-up gone (or host reset)
    digitalWrite(PIN_DP, LOW);
    pinMode(PIN_DP, OUTPUT);                                  // falling edge
    delayMicroseconds(10);
    pinMode(PIN_DP, INPUT);
    delayMicroseconds(1000 - 10 - SENSE_US);
  }
  return false;
}

// 1 = the chip took the bus, -1 = a pull-up that never dropped (a firmware is running,
// not the ROM), 0 = no acknowledgement yet. Interrupts stay off for the 16 bits (320 us): a
// stretched level is harmless, but keeping the whole packet tight is free at this length.
static int sendKeyOnce() {
  noInterrupts();
  for (int bit = 15; bit >= 0; bit--) {
    if (chipPulledUp()) {
      interrupts();
      bool ok = clockTrain();
      releaseBus();
      return ok ? 1 : -1;
    }
    digitalWrite(PIN_DM, (USB_KEY >> bit) & 1);
    delayMicroseconds(HALF_PERIOD_US - SENSE_US);
    digitalWrite(PIN_DP, HIGH);
    delayMicroseconds(HALF_PERIOD_US);
    digitalWrite(PIN_DP, LOW);
  }
  interrupts();
  return 0;
}

// D+ high before the chip was ever seen low is not an acknowledgement: it is a firmware still
// attached to the host (full-speed idle), or one that dropped off USB but left its pull-up on.
// Leave the lines alone until D+ goes low, then start keying. The pull-down keeps a line with
// nothing on it from reading the level the pad's input capacitance kept from the last drive.
// SENSE_US is too short here: the tapped cable's capacitance holds its last level longer, so a
// floating line read that fast looks high and the attempt never arms. Settle with the probe
// time instead (the host's 15k pull-downs still win on a connected, idle bus).
static bool dpReleasedHigh() {
  pinMode(PIN_DM, INPUT);
  return lineHighPulledDown(PIN_DP, PROBE_US);
}

// A released line holds its last level on the cable's capacitance for a long time, so a 3 us
// pull-down cannot separate "floating" from "driven high". Settle long enough that only a real
// pull-up keeps the line high; the chip in UBOOT pulls D+ (and D-) up.
static void probeLines() {
  bool dp = lineHighPulledDown(PIN_DP, PROBE_US);
  bool dm = lineHighPulledDown(PIN_DM, PROBE_US);
  Serial.printf("Probe: D+=%s D-=%s (HIGH = held up, LOW = free)\n",
                dp ? "HIGH" : "LOW", dm ? "HIGH" : "LOW");
}

// With the pad off, the only thing on the data lines is the computer's 15 kOhm pulldowns, so a
// weak internal pull-up that gets dragged low proves the wire reaches the PC. (A chip in UBOOT
// holds D+ up itself; run this while the pad is dark.)
static void lineTest(const char *name, uint8_t pin) {
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(200);
  int v = digitalRead(pin);
  pinMode(pin, INPUT);
  Serial.printf("  %s %-4s %s\n", name, v ? "HIGH" : "LOW",
                v ? "(open: does not reach the PC)" : "(reaches the PC)");
  if (!v) parkLow(pin);
}

static void printLineTest() {
  Serial.println("Line check, pad must be off:");
  lineTest("D+", PIN_DP);
  lineTest("D-", PIN_DM);
  releaseBus();
}

// Pretend to be a full-speed device for 30 ms: D+ high, D- low is the idle state a host sees as
// "something plugged in". If the host logs a connect, the D+ path really reaches it.
static void connectTest() {
  digitalWrite(PIN_DM, LOW);
  pinMode(PIN_DM, OUTPUT);
  digitalWrite(PIN_DP, HIGH);
  pinMode(PIN_DP, OUTPUT);
  delay(30);
  releaseBus();
  Serial.println("Connect test: D+ high for 30 ms, bus released (see host log).");
}

// A character during an attempt: 'x' aborts it, 'r' starts it over. Never queue attempts.
// Newlines are skipped instead of returned: tools that write "x\r" get both bytes in one
// packet, and returning the last one dropped the command.
static int pollSerial() {
  int c = -1;
  while (Serial.available()) {
    int b = Serial.read();
    if (b != '\r' && b != '\n') c = b;
  }
  return c;
}

static void runAttempt() {
  Serial.println("Sending USB_KEY 0x16EF - power on the SMC-PAD now...");
  releaseBus();
  statusRGB(0, 0, 0);
  bool armed = false;
  uint32_t start = millis(), keys = 0, lastBlink = 0;
  bool led = false;
  while (millis() - start < TIMEOUT_MS) {
    int c = pollSerial();
    if (c == 'x') {
      releaseBus();
      Serial.println("Aborted.");
      statusRGB(0, 0, 0);
      return;
    }
    if (c == 'r') {
      releaseBus();
      armed = false;
      start = millis();
      keys = 0;
      Serial.println("Restarted.");
    }
    if (!armed) {
      // Low for a moment is not enough: a device still attached sees the host's packets
      // toggle D+. Only a detached bus stays low.
      uint32_t lowSince = millis();
      while (!dpReleasedHigh()) {
        if (millis() - lowSince >= ARM_LOW_MS) {
          armed = true;
          break;
        }
        delayMicroseconds(50);
      }
      if (!armed) {
        delay(1);
        continue;
      }
      Serial.println("D+ low: keying.");
      driveBus();
    }
    int r = sendKeyOnce();
    if (r == 1) {
      statusRGB(0, 60, 0);                 // solid green: the key was accepted
      Serial.printf("OK: chip took the bus after %lu keys (%lu ms). Bus released.\n",
                    keys, millis() - start);
      Serial.println("The SMC-PAD should now enumerate as a UBOOT mass-storage device.");
      return;
    }
    if (r < 0) {
      Serial.println("D+ pulled up but never dropped: a firmware is running, not the ROM.");
      Serial.println("Power the pad off, then send 'r'.");
      for (int i = 0; i < 3; i++) {        // red blink
        statusRGB(60, 0, 0);
        delay(300);
        statusRGB(0, 0, 0);
        delay(300);
      }
      return;
    }
    keys++;
    if (millis() - lastBlink > 50) {
      led = !led;
      statusRGB(0, 0, led ? 60 : 0);       // blue blink: keying
      lastBlink = millis();
    }
  }
  releaseBus();
  Serial.println("Timeout: no response. Check wiring, then send 'r' to retry.");
  for (int i = 0; i < 3; i++) {            // red blink: timed out
    statusRGB(60, 0, 0);
    delay(300);
    statusRGB(0, 0, 0);
    delay(300);
  }
}

// Hold both lines low (SE0) for a while and let go: to the host this is an unplug and a replug,
// which makes it enumerate the device again after it gave up on that port.
static void replug() {
  driveBus();
  delay(300);
  releaseBus();
  Serial.println("Replugged (SE0 300 ms).");
}

void setup() {
  releaseBus();
  statusRGB(0, 0, 0);
  Serial.begin(115200);
  delay(1500);
  Serial.println("Idle: no automatic keying. Send 'r' to key, 't' line check, 'p' probe, "
                 "'d' replug, 'k' connect test.");
}

void loop() {
  int c = pollSerial();
  if (c == 'd') replug();
  if (c == 't') printLineTest();
  if (c == 'p') probeLines();
  if (c == 'k') connectTest();
  if (c == 'r') {
    statusRGB(0, 0, 0);
    runAttempt();
  }
}
