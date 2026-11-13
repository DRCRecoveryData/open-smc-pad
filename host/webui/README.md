# SMC-PAD Pocket web UI

A local page to back up, restore and flash the pad, drive its LEDs live, and rebuild the
Spectra firmware with your settings.

    python3 host/webui/server.py            # http://127.0.0.1:8765/
    python3 host/webui/server.py --offline  # UI only, no device access

It talks to `1209:0002` with `dfu-util` and never writes sector 0:

- **Backup application** — DFU alt 0 read: the app area (directory + app.bin + cfg).
  Restorable from the Restore tab.
- **Backup full flash** — DFU alt 1 read, read-only, all 512 KiB. Archival only: a raw
  restore needs the Pico ROM download flow (`host/flash_fw.sh stock` on macOS, or
  `tools/jl-uboot-tool` on Linux).
- **Restore / Upload** — DFU alt 0 write, transactional (the bootloader checks the image
  and writes the directory last; an interrupted transfer leaves the pad in the bootloader).
- **Lights** — live over raw MIDI to `hw:N,0,0`: per-pad palette colours, brightness/gate/
  pulse animations, bank switching; the pad mockup follows the real taps the sketch sends.
- **Settings** — every `SpectraConfig.h` option (channels, velocity, momentary CC, spark,
  bank wave, sleep, side buttons and the per-bank Utility colours). Save stores them in
  `work/webui/settings.json`; Save & flash rebuilds MidiFighterSpectra with `-D` overrides
  (`host/build_sketch.sh` via `SMCPAD_DEFS`/`SMCPAD_OUT`) and flashes the result.
- **Advanced: bootloader update** — DFU alt 2 with an explicit typed confirmation.
- **Leave DFU** — boots the application again after a failed write.

Backups are written to `work/webui/backups/` (gitignored); uploads are validated on arrival
and kept as `app_<timestamp>_uploaded.bin` after a successful flash. Settings-driven builds
land in `work/webui/build/MidiFighterSpectra/` and are kept as
`app_<timestamp>_settings.bin`, so `work/sketch/` is never overwritten.

The Lights tab writes plain MIDI to `/dev/snd/midiC*D0`; it needs read/write access to the
ALSA node (the desktop's uaccess ACL usually provides it). The palette and note map are the
Spectra guide's: colour velocity `1 + 6*index`, 0 clears, 121..127 forces the Utility colour.

## Entering the bootloader

From the running app the server uses the Arduino-style 1200-baud touch on the CDC port.
That needs permission for `/dev/ttyACM0`; install the one-time rule:

    sudo host/install-udev.sh

Then power-cycle the pad once. If the pad is already in DFU (SHIFT LED blinking on the
full-size board; DFU in `lsusb`), no touch is needed.

## Safety

- Only `1209:0001` (app), `1209:0002` (bootloader) and `4353:4b4d` (stock) are recognised;
  anything else is left alone. `.fwsc` containers and >512 KiB files are refused.
- App images are checked before any write: directory head CRC, entry `0x1E08120`, size
  range and the app-area CRC.
- One operation at a time; the page binds to 127.0.0.1 and rejects cross-origin POSTs.
- Tests: `python3 host/tests/test_webui.py` (offline).
