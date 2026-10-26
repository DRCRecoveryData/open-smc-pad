# Bluetooth: BLE-MIDI and the BLE runtime

## In the Arduino core (2026-10-20)

- **Runtime** `fw/smcpad/arduino/smcpad_ble.c` (API in `smcpad_rt.h`, `smcpad_ble_*`): a GATT
  server on the SDK stack. The caller gives the ATT database in BTstack's compiled format
  (entries `size, flags, handle, type (2/16 bytes), value`, no version byte, zero size ends it);
  dynamic attributes go to its read/write callbacks, which run in the BT stack's task. The
  CCCs (0x2902) are kept by the runtime (found by scanning the database), and a 0x2A05 Service
  Changed value, if present, is indicated on every connection (`ATT_OP_INDICATE`, range
  0x0001-0xFFFF). Local ATT MTU 247. It provides the hooks the SDK's BLE start calls
  (`bt_ble_init` & co., replacing `ble_hogp.c`); the console's BLE-MIDI now sits on it too.
- **Address = chip MAC with the low 3 bytes XORed with an FNV-1a hash of the database**
  (`adv_cfg_t.set_local_addr_tag = USE_SET_LOCAL_ADDRESS_TAG`). Why: macOS (CoreBluetooth)
  keeps the GATT database it first found for an address, and our first database had no Service
  Changed, so a new sketch's services were never seen (bleak listed only the old MIDI service,
  even after reconnecting). One address per database fixes it on any OS; sketches serving the
  same database (all BLEMIDI ones) keep their address and pairing.
- **ArduinoBLE** library (peripheral): GAP (name, appearance), GATT (Service Changed), then the
  sketch's services. Events are counted in the BT task and dispatched by `BLE.poll()`.
  Tested from macOS with bleak (`PadService` example): read, write, descriptor, notify.
- **BLEMIDI** library: on ArduinoBLE, MIDIUSB API. Tested with bleak (all pads, knobs as CC,
  host notes lighting pads) and with Audio MIDI Setup + CoreMIDI.

### Latency (BLEMIDI sending)

Until 2026-10-26 every message was its own notification. The SDK's send queue counts bytes
(`ATT_SEND_CBUF_SIZE`, ~500), so a sketch that sends a steady stream (GrooveBox: MIDI clock
48 times a second, aftertouch) filled it with ~45 small packets, while the radio sends one or
two per connection interval: every note waited behind the whole queue (~0.75 s), and once the
queue was full `smcpad_ble_notify` held the loop for up to 50 ms per message.

Now:

- A message goes out at once only when the stack's queue is empty (`smcpad_ble_queued()`);
  otherwise it joins the next packet, with its own timestamp (BLE-MIDI allows many messages
  per packet), and that packet leaves as soon as the previous one has gone: from the next
  `send()` or from `smcpad_yield()` (`smcpad_yield_hook`, after each `loop()` and in
  `delay()`). Packets are up to 64 bytes (and the ATT MTU) and span less than 128 ms, so the
  host can rebuild each message's time.
- `BLEMIDI.begin()` calls `BLE.setConnectionInterval(6, 12)` (7.5-15 ms, as the BLE-MIDI
  spec asks). The runtime asks for it when the central first subscribes; if it is refused,
  it falls back to 15-30 ms (Apple's accessory rule).

Host test (a fake radio sending one packet every 15 ms, GrooveBox-like load: clock, 4 tracks
on every 1/16, aftertouch, pad hits): before, a median latency of 757 ms and 60% of the
messages dropped; now, a median of 23 ms and a maximum of 30 ms, with nothing dropped.

### The CPU clock: 24 -> 96 MHz (2026-10-26)

On the pad, with a central connected, pads and LEDs lagged too. A sampling profiler (TIMER3
interrupt reading `os_current_task()`) showed where the CPU went at 24 MHz: the `systimer` task
40% of the time, Bluetooth or not, and `btstack` 27% more once connected, leaving the sketch a
quarter of the CPU; each stack call from the sketch (`ble_comm_att_check_send`,
`ble_comm_att_send_data`, `ble_op_multi_att_get_remain`) took 3-9 ms, and the loop went from
1.2 ms to 2.8 ms a turn with 15 ms stalls. `TCFG_CLOCK_SYS_HZ` and `CONFIG_BT_NORMAL_HZ` are now
96 MHz (the SDK runs this chip at 96 MHz with Bluetooth for audio decoding):

| | 24 MHz | 96 MHz |
|---|---|---|
| `smcpad_ble_notify` | 9 ms | 1.3 ms |
| `smcpad_ble_queued` | 3 ms | 0.3 ms |
| CPU left to the sketch, connected | 26% | 85% |
| GrooveBox loop, connected | 2.8 ms (max 15) | 0.18 ms (max 3.8) |
| BLE latency vs USB, GrooveBox playing | median 21, p95 28 ms (63 msg/s) | median 13, p95 22 ms (89 msg/s) |

What depended on the clock, and now does not: busy-wait loops counted in iterations
(`smcpad_spin.h`: flash driver timeouts and pauses, the USB RX FIFO wait, the key matrix settle,
the reset's USB detach), the pad velocity window (now 9 ms on TIMER3 instead of 3 scan rounds),
the ADC's start delay (6 x 8 clocks: the scan stays at ~330 rounds a second), and the RGB SPI
divider (`docs/hardware.md`). TIMER3 counts the crystal, so time was never affected (checked:
-154 ppm against the Mac). CoreTest passes 71/71. The cost is battery current.

## The console firmware

Status (2026-10-20): **works end to end with macOS.** Scan (bleak): "SMC-PAD" with the MIDI
service UUID, RSSI -40. GATT: the MIDI characteristic with read / write-without-response /
notify, empty read; BLE-MIDI written to it is parsed on the pad. Audio MIDI Setup > Bluetooth
> Connect gives a CoreMIDI port "SMC-PAD Bluetooth"; with the console's `K` the pads arrive as
note on/off pairs, notes 36..51, velocity following the pressure (44 messages in 60 s of
playing, none lost). USB (CDC console) keeps working alongside.

The stock firmware does standard **BLE-MIDI** (its image carries the service
`03B80E5A-EDE8-4B33-A751-6CE34EC4C700` and characteristic
`7772E5DB-3868-4112-A1A9-F2669D106BF3`, plus BLE OTA files). Ours:
`fw/smcpad/examples/smcpad_test/smcpad_blemidi.c`, on the SDK's BLE stack (BTstack + the
`gatt_common` module, closed controller in `btctrler.a`).

- GATT (BTstack compiled format, written by hand): GAP service with the device name, the MIDI
  service with the MIDI I/O characteristic (read = empty, write without response, notify) and
  its CCC. Advertising: flags + the 128-bit MIDI service UUID; scan response: "SMC-PAD".
  "Just works" pairing with bonding.
- Outgoing: one MIDI message per packet, header + timestamp from `jiffies_msec()`
  (`blemidi_send`). Incoming: full parser (timestamps, running status, real time; SysEx
  skipped for now), complete messages to `blemidi_rx()`.
- Start: console `Y` -> `clk_set("sys", BT_NORMAL_HZ)` (96 MHz since 2026-10-26, already the boot clock),
  `bt_pll_para`, `btstack_ble_start_before_init`, `btstack_init`; `BT_STATUS_INIT_OK` reaches
  `app_idle.c`'s event handler -> `btstack_ble_start_after_init` -> `bt_ble_init` (profile,
  advertising, enable). It replaces `modules/bt/ble_hogp.c` (HID over GATT) in the build.
- Console `K`: pads -> notes 36..51 (stock pad numbering, bottom-left = 36) over BLE-MIDI,
  velocity from the ADC value, the pad LED lit while held.
- The application grows from ~92 KB to ~160 KB with the stack linked in. USB (TinyUSB CDC)
  and the console keep working with BLE running (selftest passes).

To test from a Mac: console `Y`, then Audio MIDI Setup > Window > Show MIDI Studio >
Bluetooth (Configure Bluetooth) > Connect "SMC-PAD"; then `K` and press pads with a MIDI
monitor open.
