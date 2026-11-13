# SMC-PAD Pocket web UI

A page to back up, restore and flash the pad, drive its LEDs live, and produce
Spectra-firmware builds from your settings.

It runs entirely in the browser — nothing needs to be installed or running on the computer
except Chrome:

<https://drcrecoverydata.github.io/open-smc-pad-pocket/>

It is served straight from this repository: `.github/workflows/pages.yml` assembles
`host/webui` and deploys it to GitHub Pages on every push that touches it.

**Chrome, Edge or another Chromium browser is required.** The page uses WebUSB, Web Serial
and Web MIDI to reach the pad; Firefox and Safari implement none of them and show a warning
banner instead.

## Permissions

Chrome asks once per browser profile for each device:

- **Select serial port** — pick the SMC-PAD while it runs the app (`1209:0001`). The port is
  used for the 1200-baud touch that reboots the pad into the bootloader.
- **Select DFU device** — pick the SMC-PAD while it is in bootloader mode (`1209:0002`).
  This grants access for all later writes and backups.
- **MIDI** — grant the MIDI permission when the browser asks; it is needed for the Lights tab.

On the very first DFU operation the page sends the 1200-baud touch, the pad reboots into
the bootloader, and the operation stops because Chrome still needs the chooser. Pick
**Select DFU device** while the pad shows the bootloader (it stays there), then run the
operation again. Later operations reuse the grant.

On Linux the desktop's uaccess ACL usually covers the pads; if Chrome cannot open the device,
install the one-time udev rules and power-cycle the pad:

    sudo host/install-udev.sh

## What each tab does

It talks to `1209:0002` through standard DFU 1.1 and never writes sector 0:

- **Backup** — DFU alt 0 read: the app area (directory + app.bin + cfg), saved as
  `app_<timestamp>.bin` in Downloads, or DFU alt 1 read, read-only, all 512 KiB, saved as
  `flash_<timestamp>.bin`. The page shows the SHA-256 of each download. The full dump is
  archival only: a raw restore needs the Pico ROM download flow (`host/flash_fw.sh stock` on
  macOS, or `tools/jl-uboot-tool` on Linux).
- **Restore / Upload** — DFU alt 0 write, transactional (the bootloader checks the image and
  writes the directory last; an interrupted transfer leaves the pad in the bootloader, which
  this page can flash again). The file is validated in the page before any USB traffic: a
  `.fwsc`, a full-flash dump, a bad directory CRC, a wrong entry (`0x1E08120`), an out-of-range
  size or an app-area CRC mismatch is refused.
- **Lights** — live over Web MIDI (no SysEx): per-pad palette colours, brightness/gate/pulse
  animations, bank switching; the pad mockup follows the real taps the sketch sends. The
  palette is the Spectra guide's: colour velocity `1 + 6*index`, 0 clears, 121..127 forces the
  Utility colour.
- **Settings** — every `SpectraConfig.h` option (channels, velocity, momentary CC, spark, bank
  wave, sleep, side buttons and the per-bank Utility colours). Save keeps them in this
  browser's `localStorage`; "Copy settings JSON" hands them to the firmware build below.
- **Debrick** — recovers a pad that will not boot: shows the APP / DFU / no-pad state,
  reboots a running pad into the bootloader, restores the shipped default-settings application
  (`recovery/app.bin`, fetched same-origin with its size and sha256 checked in the page) and
  flashes the shipped bootloader package (`recovery/boot_update.bin`, alt 2) — both behind the
  typed-WRITE dialog. A pad whose bootloader is gone sits in the chip's ROM download mode
  (`4c4a:2542`), a USB mass-storage device that Chrome never exposes to web pages; the tab
  gives the exact Pico + `host/maskrom.py` steps for that state instead.
- **Advanced** — bootloader update through DFU alt 2 with an explicit typed confirmation, and
  **Leave DFU** to boot the application after a failed write.
- **Status** — mode (APP / DFU), MIDI and serial ports, bootloader version, and the buttons
  that grant device access.

## Firmware builds

Settings are compile-time `-DSPECTRA_*` defines, so firmware cannot be compiled in a browser.
The Settings tab links to the `firmware.yml` workflow: run it with the
copied settings JSON in `settings_json`, let GitHub Actions build with the JieLi toolchain,
download `MidiFighterSpectra.bin` from the finished run, and flash it on the Upload tab.
Backups and settings JSON stay in the browser and Downloads unless you paste them into the
workflow yourself.

## Recovery images

The Debrick tab fetches `recovery/app.bin` and `recovery/boot_update.bin` from this site, checks
their size and sha256 against `recovery/manifest.json`, and runs the same validators as a picked
file before any USB traffic. `app.bin` is the default-settings build from the firmware workflow
(`b11926ea…`); `boot_update.bin` is the alt 2 package for `smcpad-boot 0109` (`b5a5c951…`).
`recovery/README.md` documents how to regenerate both, and `host/tests/test_webui.py` pins them
so they cannot drift unnoticed. The factory image is device data and is never published.

## Safety

- Only `1209:0001` (app) and `1209:0002` (bootloader) are recognised; device choosers are
  filtered to those ids. `.fwsc` containers and files of 512 KiB or more are refused before
  any write.
- Only the DFU alt the operation needs is written: alt 0 for app restores, alt 2 for a
  bootloader update, alt 1 never. The Debrick tab adds no other write path; its ROM-mode section
  is instructions for the host tools, because the browser cannot reach that device.
- One operation at a time; reads can be cancelled, writes run to completion or leave the pad
  in the bootloader.
- No server is involved: the page is plain static files from GitHub Pages. The optional
  `python3 host/webui/server.py` remains for offline use and scripting, and serves the same
  page at <http://127.0.0.1:8765/>.
- Tests: `python3 host/tests/test_webui.py` and `node --test host/tests/test_smcpad_js.mjs`.
