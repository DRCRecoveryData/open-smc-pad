// jl-s3-bridge: keeps the SMC-PAD permanently attached to the ESP32-S3.
//
// The S3 is the USB host to the pad on its native USB port (GPIO19/20) and talks to the PC
// over UART0 (the "UART" USB-C port, CP210x) at 921600 with COBS-framed messages. Two boot
// modes, selected with MSG_MODE and stored in NVS:
//
//   midi  - host starts at boot; the pad's MIDI interface is forwarded to the PC.
//   uboot - the host PHY stays off; the keyer (GPIO4/5 taps, or the native pins with
//           BRIDGE_KEY_ON_NATIVE_PINS) sends USB_KEY 0x16EF to a pad that is powered on,
//           runs the SOF clock train, and only then starts the host. The ROM comes up as a
//           mass-storage device whose vendor SCSI commands are proxied to jl-uboot-tool.
//
// Wiring: pad USB-C -> S3 native USB port; PC -> S3 UART port; key taps (if used) on the
// pad's D+/D- with GND. The S3 cannot source VBUS on its native port.
//
// Console: none. All text goes out as MSG_LOG frames to the relay.

#include <USBHost.h>

#include "bridge_config.h"
#include "keyer.h"
#include "link.h"
#include "midi_bridge.h"
#include "msc_proxy.h"
#include "state.h"
#include "status.h"
#include "usb_host.h"

void setup() {
  statusInit();
  stateInit();
  keyerInit();
  midiInit();
  mscInit();
  linkInit();
  if (g_state.mode == MODE_MIDI) usbHostBegin();
  stateEmit(true);
}

void loop() {
  USBHost.task();
  keyerTick();
  midiTick();
  mscTick();
  stateTick();
  delay(1);
}
