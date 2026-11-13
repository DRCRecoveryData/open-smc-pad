# jl-usbkey-s3

ESP32-S3-DevKitC-1 port of [`../jl-usbkey`](../jl-usbkey). Same USB_KEY protocol, same console
commands, same behaviour; only the pin API and the status LED differ.

## Wiring

| DevKitC-1 | tapped cable |
|---|---|
| GPIO4 | USB D+ (green) |
| GPIO5 | USB D- (white) |
| GND | USB GND (black) |

Direct wires, no series resistors. The SMC-PAD stays plugged into the computer through the
tapped cable; the S3 sits in parallel on the data lines like the Pico. Do not use GPIO19/20
(the native USB port) or the strapping pins 0/3/45/46.

## Console

The console is the board's **UART** USB-C port (CP210x), `115200 8N1`. On the S3, `Serial` is
UART0 on GPIO43/44, which the CP210x is wired to, so keep *USB CDC On Boot: Disabled* (the
default for `esp32:esp32:esp32s3`). The native USB port stays free.

## Status LED

The on-board WS2812 is on GPIO48 on DevKitC-1 v1.0 and GPIO38 on v1.1. The sketch drives both
pins through the core's `rgbLedWrite()`, so either revision lights and the other pin only
outputs the same harmless pulse train.

- blue blink: keying
- solid green: the chip took the USB_KEY
- red blink: timed out, or a firmware is running (D+ pulled up and never dropped)

## Build and upload

    tools/arduino-cli/arduino-cli compile --fqbn esp32:esp32:esp32s3 hw/jl-usbkey-s3
    tools/arduino-cli/arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/ttyUSB0 hw/jl-usbkey-s3

The UART port is `/dev/ttyUSB*` on Linux and `/dev/cu.usbserial-*` on macOS. It needs the same
serial access as the Pico's CDC port; this machine runs the upload under `sudo -n` because the
user is not in `dialout`.

## Use

Same as the Pico, except that the sketch does not key at boot: it idles on the bus, so the pad
can stay wired up permanently and only a keyed attempt touches the data lines. Open the
console, send `r`, wait for `D+ low: keying.`, then power the pad on. `x` aborts, `d` sends SE0
for 300 ms, `t` checks the tap with the pad off, `p` probes both lines, `k` holds D+ high for
30 ms. See [`docs/flash-layout.md`](../../docs/flash-layout.md) for the full procedure and
pitfalls.
