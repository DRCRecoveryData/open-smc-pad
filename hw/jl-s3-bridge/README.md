# jl-s3-bridge

Keeps the SMC-PAD permanently attached to an ESP32-S3. The S3 is the USB **host** to the pad
on its native port, and the PC talks to the S3 over its UART port. No cable ever moves
between flashing a firmware and using it as a MIDI controller.

```
pad USB-C ──► S3 native USB (GPIO19/20, host PHY, full speed)
S3 UART0 (GPIO43/44, 921600) ──► CP210x or CH343 ──► PC
key taps (optional, GPIO4/5) ──► pad D+/D- (the tapped cable)
```

The PC sees a SCSI socket for the UBOOT flow (`host/s3_relay.py` +
`tools/jl-uboot-tool/scsiio/relayport.py`) and, with `--midi`, RTMidi virtual ports carrying the
pad's USB-MIDI. Architecture, wire protocol and the bring-up checklist:
[`docs/s3-bridge.md`](../../docs/s3-bridge.md).

## Wiring

| from | to | note |
|---|---|---|
| pad USB-C | S3 **native** USB-C | the S3 is the host; A-to-C/C-to-C cable as appropriate |
| PC | S3 **UART** USB-C | CP210x or CH343, `/dev/ttyUSB*` or `/dev/ttyACM*`, 921600 8N1 |
| S3 GPIO4 | pad USB D+ | only when keying through the taps (`BRIDGE_KEY_ON_NATIVE_PINS 0`) |
| S3 GPIO5 | pad USB D- | idem |
| S3 GND | pad USB GND | idem |

The devkit's native port cannot source VBUS. The pad is battery powered, so it normally does
not need it; if the ROM does not enumerate, feed 5 V from the devkit `5V` pin through a USB-C
breakout to the pad.

The taps stay wired to the same cable the previous keyer used. With
`BRIDGE_KEY_ON_NATIVE_PINS 1` in [`bridge_config.h`](bridge_config.h) the keyer bit-bangs
GPIO19/20 instead, which needs no extra wires but only works while the host PHY is off (it is,
in UBOOT mode, until the key is acknowledged).

## Modes

Two boot modes, stored in NVS (`Preferences("s3bridge")`) and switched by the host:

| mode | boot behaviour |
|---|---|
| `midi` (default) | `USBHost.begin()` in `setup()`; the pad enumerates as `4353:4b4d` and its MIDI cables are forwarded |
| `uboot` | host PHY stays off; the keyer keys on command; after the ACK and the SOF clock train the host starts and the ROM mounts as `4C4A:2542` (USB MSC) |

`host/s3_relay.py mode midi|uboot` writes the mode and reboots the S3. The reboot does not
disturb the UART bridge tty. Switching back to MIDI needs no replug: after the ROM's `run_app` the
pad re-enumerates and the running host mounts it as MIDI.

## Console and LED

There is no text console on UART0 — the link owns it. Everything the firmware would print goes
out as `MSG_LOG` frames, which the relay prints as `[s3] ...`.

The on-board WS2812 (GPIO48 on v1.0, GPIO38 on v1.1, both driven) shows the keyer:

- blue blink: keying
- solid green: the chip took the USB_KEY (host starting)
- red blink: timeout, or a firmware is running (D+ stays pulled up)

## Build and upload

Needs arduino-esp32 **3.3.12** (IDF 5.5; older cores lack `USBHost`). Keep
*USB Mode: USB-OTG (TinyUSB)* and *USB CDC On Boot: Disabled* — the defaults for
`esp32:esp32:esp32s3`.

    arduino-cli compile --fqbn esp32:esp32:esp32s3 hw/jl-s3-bridge
    arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/ttyUSB0 hw/jl-s3-bridge

Upload goes through the UART bridge port (`/dev/ttyUSB*` or `/dev/ttyACM*`), so the pad may
stay plugged into the native port.

## Limits

- Full-speed host only; one device at a time (the pad).
- SCSI proxying carries one command of up to 1024 bytes; jl-uboot-tool uses 512.
- The MIDI host buffers are 64 bytes per direction as built into the prebuilt TinyUSB library;
  the bridge drains them in a loop instead of redefining `CFG_TUH_MIDI_*`.
- `tuh_msc_scsi_command` sends the command data as one bulk transfer; the ROM tolerates that,
  but a stall mid-transfer is answered with one clear-and-retry.
- No automatic keying: a stock pad is never disturbed by accident.
