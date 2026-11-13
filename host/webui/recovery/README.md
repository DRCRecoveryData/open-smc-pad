# Recovery images served by the Debrick tab

The page fetches these same-origin, checks the size and sha256 from `manifest.json`, and runs
the same validators as a file picked on the Restore/Upload tabs before any USB traffic.

| file | size | sha256 |
|---|---|---|
| `app.bin` | 149128 | `b11926ea0d01b9ad524d559d6e8b067c913bd22fb518c54bacac8079d638b824` |
| `boot_update.bin` | 28672 | `b5a5c951158ce4cdfd2005aabf8236706c19325f95b1700ab653da96e1585ae0` |

`app.bin` is the application area built by `.github/workflows/firmware.yml` with default
settings; this copy is the byte-identical read-back of the flashed artifact. `boot_update.bin`
is the DFU alt 2 package for `smcpad-boot 0109` (`BOOT_VERSION` in `fw/boot/boot.h`).

## Regenerating

Application (when the default settings or the sketch change): run the firmware workflow, flash
its `MidiFighterSpectra.bin` through the site (or `host/dfu_flash.sh`), back the application up
again, and copy the untouched read-back over `app.bin`. The read-back must validate with
`host/webui/device.py:validate_app_area`.

Bootloader (when `BOOT_VERSION` changes):

    fw/boot/build.sh && host/package_fw.sh boot
    cp work/flash/boot_update.bin host/webui/recovery/boot_update.bin

Then update `manifest.json` and the pin in `host/tests/test_webui.py` (`RecoveryAssetsTest`) in
the same commit — the test fails if the files drift. Never put anything from `dumps/` here: the
factory image is device data and stays local.
