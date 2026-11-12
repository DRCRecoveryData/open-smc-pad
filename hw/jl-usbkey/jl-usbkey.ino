// jl-usbkey: forces a JieLi chip (SMC-PAD: AC6966C / BR25) into its MaskROM USB
// download mode ("UBOOT") by sending the USB_KEY on the USB data lines at power-up.
// Ref: tools/jielie/isp/usb/usb-key.md
//
// Wiring (Raspberry Pi Pico, arduino-pico core):
//   GP2 ---- USB D+  (green)   key clock
//   GP3 ---- USB D-  (white)   key data
//   GND ---- USB GND (black)
// Direct wires, no series resistors.
// The SMC-PAD stays connected to the computer through the tapped cable.
//
// Use: open the serial monitor (or just watch the LED), then power on the SMC-PAD.
// RGB status on the RP2040-Zero's WS2812 (GP16): blue blink = keying, solid green = the
// chip took the USB_KEY, red blink = timed out. A plain Pico's GP25 LED, when the variant
// declares one, mirrors on/off.
// Send 'r' over serial (or press the board's RESET button) to start an attempt; 'x' aborts
// one; 'd' replugs the pad as far as the host can tell; 't' checks with the pad off whether
// the data lines really reach the computer; 'k' pulses D+ high for 30 ms.

#include <Arduino.h>

constexpr uint PIN_WS2812 = 16;       // RP2040-Zero: its only LED, a WS2812

constexpr uint PIN_DP = 2;            // D+ : clock, data latched on rising edge
constexpr uint PIN_DM = 3;            // D- : data, MSB first
constexpr uint16_t USB_KEY = 0x16EF;
constexpr uint32_t HALF_PERIOD_US = 10;   // ~50 kHz clock
constexpr uint32_t SENSE_US = 3;          // settle time before reading released D+
constexpr uint32_t PROBE_US = 300;        // pull-down settle that outlasts cable capacitance
constexpr uint32_t TIMEOUT_MS = 600000;
constexpr uint32_t ARM_LOW_MS = 10;       // D+ must stay low this long before keying
constexpr uint32_t TRAIN_PULSES = 150;    // worst-case calibration window, 1 ms per pulse

static void releaseBus() {
  gpio_set_dir(PIN_DP, GPIO_IN);
  gpio_set_dir(PIN_DM, GPIO_IN);
}

static void driveBus() {
  gpio_put(PIN_DP, 0);
  gpio_put(PIN_DM, 0);
  gpio_set_dir(PIN_DP, GPIO_OUT);
  gpio_set_dir(PIN_DM, GPIO_OUT);
}

#ifndef F_CPU
#define F_CPU 133000000UL
#endif

// subs+bne take 3 cycles per iteration on this core.
static inline void spinNs(uint32_t ns) {
  uint32_t n = (uint32_t)(((uint64_t)F_CPU * ns) / 3000000000ULL);
  asm volatile("1: sub %0, #1\n\tbne 1b" : "+l"(n) : : "cc");
}

// Status colour on the WS2812 (GRB, 1.25 us bits, then the reset gap); a plain LED on the
// variant's LED_BUILTIN pin, when there is one, mirrors on/off.
static void statusRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
  gpio_set_dir(PIN_WS2812, GPIO_OUT);
  noInterrupts();
  for (int i = 23; i >= 0; i--) {
    bool one = (grb >> i) & 1;
    gpio_put(PIN_WS2812, 1);
    spinNs(one ? 750 : 350);
    gpio_put(PIN_WS2812, 0);
    spinNs(one ? 500 : 900);
  }
  interrupts();
  delayMicroseconds(300);
#ifdef LED_BUILTIN
  digitalWrite(LED_BUILTIN, (r | g | b) != 0);
#endif
}

// D+ is only ever released while the clock is low, so seeing it high means the chip
// has acknowledged the key and enabled its 1.5k pull-up for SOF calibration. Sense with the
// internal pull-down on: a released line with nothing on it otherwise keeps its last level in
// the pad's input capacitance and reads high for minutes.
static bool chipPulledUp() {
  gpio_set_dir(PIN_DP, GPIO_IN);
  gpio_pull_down(PIN_DP);
  busy_wait_us_32(SENSE_US);
  bool up = gpio_get(PIN_DP);
  if (!up) {
    gpio_put(PIN_DP, 0);
    gpio_set_dir(PIN_DP, GPIO_OUT);
  }
  gpio_disable_pulls(PIN_DP);
  return up;
}

// After the acknowledge the chip pulls D+ up and measures the 1 ms frame clock from falling
// edges on that line before it brings its USB up. An idle host port sends no SOFs, so the chip
// locks onto noise and its PLL comes up wrong -- the host then never enumerates it. Give it the
// edges ourselves: 10 us low every 1 ms, until it drops its pull-up (calibration over).
// True only if the pull-up went away: a running firmware keeps it up, so that is no success.
static bool clockTrain() {
  for (uint32_t i = 0; i < TRAIN_PULSES; i++) {
    gpio_set_dir(PIN_DP, GPIO_IN);
    gpio_pull_down(PIN_DP);
    busy_wait_us_32(SENSE_US);
    if (!gpio_get(PIN_DP)) {                // pull-up gone (or host reset): done
      gpio_disable_pulls(PIN_DP);
      return true;
    }
    gpio_disable_pulls(PIN_DP);
    gpio_put(PIN_DP, 0);
    gpio_set_dir(PIN_DP, GPIO_OUT);         // falling edge
    busy_wait_us_32(10);
    gpio_set_dir(PIN_DP, GPIO_IN);
    busy_wait_us_32(1000 - 10 - SENSE_US);
  }
  return false;
}

// 1 = the chip took the bus, -1 = a pull-up that never dropped (a firmware is running,
// not the ROM), 0 = no acknowledgement yet.
static int sendKeyOnce() {
  for (int bit = 15; bit >= 0; bit--) {
    if (chipPulledUp()) {
      bool ok = clockTrain();
      releaseBus();
      return ok ? 1 : -1;
    }
    gpio_put(PIN_DM, (USB_KEY >> bit) & 1);
    busy_wait_us_32(HALF_PERIOD_US - SENSE_US);
    gpio_put(PIN_DP, 1);
    busy_wait_us_32(HALF_PERIOD_US);
    gpio_put(PIN_DP, 0);
  }
  return 0;
}

// D+ high before the chip was ever seen low is not an acknowledgement: it is a firmware still
// attached to the host (full-speed idle), or one that dropped off USB but left its pull-up on.
// Leave the lines alone until D+ goes low, then start keying. The pull-down keeps a line with
// nothing on it from reading the level the pad's input capacitance kept from the last drive.
// SENSE_US is too short here: the tapped cable's capacitance holds its last level longer,
// so a floating line read that fast looks high and the attempt never arms. Settle with the
// probe time instead (the host's 15k pull-downs still win on a connected, idle bus).
static bool dpReleasedHigh() {
  gpio_set_dir(PIN_DP, GPIO_IN);
  gpio_set_dir(PIN_DM, GPIO_IN);
  gpio_pull_down(PIN_DP);
  busy_wait_us_32(PROBE_US);
  bool up = gpio_get(PIN_DP);
  gpio_disable_pulls(PIN_DP);
  return up;
}

// A released line holds its last level on the cable's capacitance for a long time, so a 3 us
// pull-down cannot separate "floating" from "driven high". Settle long enough that only a real
// pull-up keeps the line high; the chip in UBOOT pulls D+ (and D-) up.
static void probeLines() {
  gpio_set_dir(PIN_DP, GPIO_IN);
  gpio_pull_down(PIN_DP);
  busy_wait_us_32(PROBE_US);
  bool dp = gpio_get(PIN_DP);
  gpio_disable_pulls(PIN_DP);
  gpio_set_dir(PIN_DM, GPIO_IN);
  gpio_pull_down(PIN_DM);
  busy_wait_us_32(PROBE_US);
  bool dm = gpio_get(PIN_DM);
  gpio_disable_pulls(PIN_DM);
  Serial.printf("Probe: D+=%s D-=%s (HIGH = held up, LOW = free)\n",
                dp ? "HIGH" : "LOW", dm ? "HIGH" : "LOW");
}

// With the pad off, the only thing on the data lines is the computer's 15 kOhm pulldowns, so a
// weak internal pull-up that gets dragged low proves the wire reaches the PC. (A chip in UBOOT
// holds D+ up itself; run this while the pad is dark.)
static void lineTest(const char *name, uint pin) {
  gpio_set_dir(pin, GPIO_IN);
  gpio_pull_up(pin);
  busy_wait_us_32(200);
  int v = gpio_get(pin);
  gpio_disable_pulls(pin);
  Serial.printf("  %s %-4s %s\n", name, v ? "HIGH" : "LOW",
                v ? "(open: does not reach the PC)" : "(reaches the PC)");
  if (!v) {
    gpio_put(pin, 0);                       // no pull-up left after disable: park it low
    gpio_set_dir(pin, GPIO_OUT);
  }
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
  gpio_put(PIN_DM, 0);
  gpio_set_dir(PIN_DM, GPIO_OUT);
  gpio_put(PIN_DP, 1);
  gpio_set_dir(PIN_DP, GPIO_OUT);
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
        busy_wait_us_32(50);
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
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  gpio_init(PIN_DP);
  gpio_init(PIN_DM);
  gpio_disable_pulls(PIN_DP);
  gpio_disable_pulls(PIN_DM);
  releaseBus();
  Serial.begin(115200);
  delay(1500);
  runAttempt();
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
