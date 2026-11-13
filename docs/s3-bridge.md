# ESP32-S3 USB-host bridge

The pad is permanently plugged into an ESP32-S3, and the S3 is permanently plugged into the
PC's UART port. The S3 is the USB **host** to the pad, so it can present the pad's MIDI to the
PC and proxy the UBOOT mass-storage protocol — without ever moving a cable.

- Firmware: [`hw/jl-s3-bridge`](../hw/jl-s3-bridge) (Arduino-esp32 3.3.12, `USBHost` + prebuilt
  TinyUSB host with `CFG_TUH_MIDI` and `CFG_TUH_MSC`).
- PC daemon: [`host/s3_relay.py`](../host/s3_relay.py).
- Tool backend: [`tools/jl-uboot-tool/scsiio/relayport.py`](../tools/jl-uboot-tool/scsiio/relayport.py).

```
pad USB-C ──► S3 native port (GPIO19/20, full-speed host)
S3 UART0 (GPIO43/44, 921600) ──► CP210x or CH343 ──► ttyUSB0 / ttyACM0
                                            │
                        host/s3_relay.py ───┤── Unix socket (macport wire format) ──► jluboottool
                                            └── RTMidi virtual ports ──► DAW / midi_monitor.py
```

## Boot modes

The host PHY occupies GPIO19/20, and there is no public `USBHost.end()`, so keying and hosting
cannot run at the same time. Two modes, stored in NVS and selected by the relay:

- **`midi`** (default) — `USBHost.begin()` runs from `setup()`. Plug the pad in (or power it on)
  and it enumerates as `4353:4b4d`; the MIDI interfaces are forwarded.
- **`uboot`** — the host stays off. The keyer drives `USB_KEY 0x16EF` on D+/D- when armed, waits
  for the pull-up ACK, sends the SOF clock train, and only then calls `USBHost.begin()`. The
  ROM mounts as `4C4A:2542` (USB MSC) and vendor CDBs are proxied to jl-uboot-tool.

Switching to `uboot` reboots the S3, which does not disturb the UART bridge tty. Switching back to
`midi` needs no reboot either: the ROM's `run_app` makes the pad re-enumerate as MIDI and the
already-running host mounts it.

The keyer uses the proven GPIO4/5 taps by default so the existing tapped cable keeps working;
`BRIDGE_KEY_ON_NATIVE_PINS 1` bit-bangs GPIO19/20 instead (valid only while the PHY is off).
It never keys by itself: a stock pad is untouched unless `keyer arm` is sent in `uboot` mode.

## Link layer

UART0 at 921600 8N1, frames delimited by `0x00`, COBS-encoded, and each body is

    u8 type | u8 seq | u16 len (LE) | payload[len] | u16 crc16

with CRC-16/CCITT-FALSE (init `0xFFFF`, poly `0x1021`; check value `0x29B1` for `123456789`).
Max payload 2048. A frame with bad COBS or CRC is dropped; the decoder resynchronises on the
next `0x00`. Sequence numbers are informational only. `HELLO` is sent by the relay at startup;
the S3 answers `HELLO_ACK` and a full `STATE`.

| type | dir | payload |
|---|---|---|
| `0x01` HELLO | PC→S3 | `u16 proto, u16 want_baud, u8 flags` |
| `0x02` HELLO_ACK | S3→PC | `u16 proto, u8 mode, u8 fw, u16 max_payload` |
| `0x10` STATE | S3→PC | `u8 mode, u8 pad, u16 vid, u16 pid, u8 rx_cables, u8 tx_cables, u8 keyer, u8 rsvd` |
| `0x20` MIDI_IN | S3→PC | N × 4-byte USB-MIDI packets (cable in the high nibble) |
| `0x21` MIDI_OUT | PC→S3 | idem |
| `0x30` SCSI_REQ | PC→S3 | `u32 req_id, u8 dir, u8 cdb_len, u16 data_len, u32 timeout_ms, u8 cdb[16], [data_out]` |
| `0x31` SCSI_RSP | S3→PC | `u32 req_id, u8 status, u8 sense_len, u16 data_len, u32 residue, data_in, sense` |
| `0x40` KEYER | PC→S3 | `u8 op, u8 rsvd, u32 wait_ms` |
| `0x41` KEYER_ST | S3→PC | `u8 phase, u8[3] rsvd, u32 keys, u32 ms` |
| `0x42` MODE | PC→S3 | `u8 mode` (`0` midi, `1` uboot; the S3 saves and restarts) |
| `0x50`/`0x51` PING/PONG | ↔ | echoed verbatim |
| `0x60` LOG | S3→PC | UTF-8 text (there is no other console) |

Constants: `mode` 0 midi / 1 uboot; `pad` 0 unknown / 1 midi / 2 uboot; keyer op 0 disarm /
1 arm / 2 replug (SE0 300 ms) / 3 host; keyer phase 0 idle / 1 armed / 2 keying / 3 ok /
4 timeout / 5 blocked.

`STATE` is emitted on change and once a second. `KEYER_ST` is emitted while an attempt is
active. The relay prints both as one-line JSON on stderr.

## SCSI proxying

`dir` is 0 none, 1 out (PC→pad), 2 in (pad→PC). One request is outstanding at a time; a second
is answered with status `SCSI_BUSY` (6). The firmware builds its own `msc_cbw_t`
(signature `USBC`, fresh tag, `total_bytes = data_len`, matching direction, `cmd_len = 16`, the
16 CDB bytes) and hands it to `tuh_msc_scsi_command`, which passes it to the device verbatim.
The completion callback posts CSW status and residue back to the bridge task. On
`MSC_CSW_STATUS_FAILED` the bridge issues `REQUEST SENSE` (CDB `0x03`, 18 bytes) and returns the
sense data; jl-uboot-tool then reports it like it does on `/dev/sg`.

| status | meaning |
|---|---|
| 0 | ok |
| 1 | command failed (CSW failed/phase error; sense attached) |
| 2 | transport error |
| 3 | pad not in UBOOT mode (nothing mounted as MSC) |
| 4 | unsupported request (too large, indirect I/O) |
| 5 | timeout |
| 6 | busy |

`data_len` is limited to 1024 bytes (`MAX_SCSI_DATA`); jl-uboot-tool uses 512-byte chunks, so
every existing command works. While the pad is in MIDI mode every SCSI request fails with
status 3, which is exactly what `JL_MSCDevice.open()`'s INQUIRY retry loop expects.

## MIDI passthrough

The host's MIDI interface receives 4-byte USB-MIDI event packets; the bridge forwards them
untouched (cable nibble included) as `MIDI_IN`. The other direction takes `MIDI_OUT` frames into
a 1024-byte ring that the loop drains into `tuh_midi_packet_write_n` and a per-interface flush.
The relay converts packets to complete messages for mido and back, including SysEx split across
CIN 4/5/6/7 packets. Cable counts from the mount callback become STATE and decide how many
virtual ports are opened (up to 4).

## PC side

One process owns the tty (flock). Everything else talks to it over two Unix sockets under
`/run/user/$UID/smcpad-s3.{sock,ctl}`:

    host/s3_relay.py serve --midi        # daemon: SCSI socket, control socket, MIDI ports
    host/s3_relay.py status              # prints STATE + last keyer phase as JSON
    host/s3_relay.py mode uboot          # switch mode (the S3 restarts)
    host/s3_relay.py mode midi
    host/s3_relay.py keyer arm           # key on the next pad power-on (uboot mode only)
    host/s3_relay.py keyer replug        # D+/D- SE0 for 300 ms
    host/s3_relay.py keyer disarm

`status`, `mode` and `keyer` first try the control socket and otherwise take the tty briefly, so
they work with or without the daemon (stop the daemon before using `--device serial:` directly,
because of the flock).

The SCSI socket speaks the macport wire format used by `host/jl_usb_bridge.py`:

    req: u8 dir | u8 cdb_len | u32 len | u32 timeout_ms | cdb | [out data]      (<BBII)
    rsp: u8 status | u32 residue | u32 n | data_in | u32 m | sense[m]           (<BII, <I)

so the tool needs no special case. It identifies the chip itself: INQUIRY, chip key and
online-device info are printed as soon as it opens. Commands are shell-style positional
arguments, same as `docs/flash-layout.md`:

    tools/jl-uboot-tool/jluboottool.py --device sock:/run/user/$UID/smcpad-s3.sock \
        "read 0x0 0x10000 /tmp/pad_64k.bin"

or, without the daemon (UBOOT only, no MIDI; the daemon holds the tty):

    ... --device serial:/dev/ttyUSB0@921600 "read 0x0 0x100 /tmp/check.bin"   # or ttyACM0

`scsiio/__init__.py` dispatches on the `sock:` / `serial:` prefixes; `--device` is otherwise
unchanged, and `jldevfind.py` does not need to know about this backend.

## Build and tests

    arduino-cli compile --fqbn esp32:esp32:esp32s3 hw/jl-s3-bridge     # USB CDC off, 3.3.12
    python3 -m unittest discover -s host/tests                          # needs pyserial

`host/tests` covers the frame codec (round trip, zeros, max payload, CRC corruption, garbage
resync) and the tool backends against a fake S3 on a pty and a fake relay socket.

## Bring-up checklist (needs a working S3)

1. Pad off, S3 in `midi` mode: `host/s3_relay.py status` answers, `pad: unknown`, no bus drive.
2. Pad on: `STATE` shows `pad: midi`, `vid 0x4353 pid 0x4b4d` and the cable counts. With
   `serve --midi`, `host/midi_monitor.py` sees and can send MIDI through the virtual ports.
3. `mode uboot`; `keyer arm`; power the pad on. `STATE`/`KEYER_ST` go `armed` → `keying` → `ok`
   and the pad enumerates as `4C4A:2542`.
4. With `serve` running, open `jluboottool --device sock:/run/user/$UID/smcpad-s3.sock`
   (the INQUIRY/chip-key banner proves the link), then `"read 0x0 0x10000 /tmp/pad_64k.bin"`
   and compare it with `dumps/original_physical_full.bin`. Read-only.
5. Full backup and hash compare. Restoring or flashing stays a manual decision, as before.
6. If the ROM does not enumerate without VBUS, feed 5 V to the pad from the devkit `5V` pin
   through a USB-C breakout (the S3's native port is input-only).

`host/s3_bringup.sh` runs the parts that need no hardware and prints this list.

## Risks and limits

- Keying on GPIO19/20 depends on the PHY being genuinely off; that path is unproven on this
  core, which is why GPIO4/5 taps are the default.
- `tuh_msc_scsi_command` issues the whole transfer as one bulk-IN/OUT with no ZLP. The ROM has
  not been seen to care, but a mid-transfer stall has no separate recovery path yet.
- TinyUSB host MIDI buffers are 64 bytes as compiled into the prebuilt library; drains happen
  in a loop, and `CFG_TUH_MIDI_*` cannot be raised without rebuilding that library.
- Full-speed host only, one device on the bus.
- The current devkit does not boot any firmware; everything above is compile/test-verified and
  awaits a replacement board.
