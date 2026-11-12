#!/usr/bin/env python3
"""Recovery: put the *fixed* installer and staging area into free flash through MaskROM
download mode (hw/jl-usbkey + jl-uboot-tool), then power-cycle. Sector 0 is not touched here:
it still points at INSTALL_DIR from install_from_stock.py, so the uboot runs the new installer,
which erases the stock firmware, writes the bootloader and points sector 0 at 0x1000.

    python3 host/rom_install.py --dry-run      # build, check sector 0, write nothing
    python3 host/rom_install.py                # erase + write + read back

The installer and staging area are built exactly as install_from_stock.py builds them, from
the verified stock dump (a VM/PRCT/cfg_tool template is all it needs).
"""
import argparse
import pathlib
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import bootpkg                                       # noqa: E402
import jlimage as J                                  # noqa: E402

TOOL = ROOT / "tools" / "jl-uboot-tool" / "jluboottool.py"
OUT = ROOT / "work" / "recover"
DUMP = ROOT / "dumps" / "original_physical_full.bin"

SECTOR = 0x1000
DEST, INSTALL_DIR, STAGE, STAGE_END = bootpkg.DEST, bootpkg.INSTALL_DIR, bootpkg.STAGE, bootpkg.STAGE_END
DIAG_PAGE, DIAG_MAGIC = 0x4C000, 0x31474449


def say(msg):
    print(msg, flush=True)


def die(msg):
    sys.exit(f"!! {msg}")


def rom(args, cmds):
    cmd = [sys.executable, str(TOOL)]
    if args.device:
        cmd += ["--device", args.device]
    r = subprocess.run(cmd + cmds, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode:
        die(f"jluboottool failed ({r.returncode})")


def rom_read(args, addr, n, path):
    rom(args, [f"read {addr:#x} {n:#x} {path}", "exit"])
    return path.read_bytes()


# ------------------------------------------------------------------ sector 0 (as install_from_stock.py)

def sector0_keystream():
    k = bytearray(SECTOR)
    for off in range(0, SECTOR, 256):
        J.enc_cipher(k, off, 256)
    return bytes(k)


def entries(phys):
    out = []
    for off in range(0x20, 0x200, 32):
        e = bytearray(phys[off:off + 32])
        J.enc_cipher(e, 0, 32)
        if J.crc16(bytes(e[2:])) != struct.unpack_from("<H", e)[0]:
            break
        out.append((off, bytes(e)))
    return out


def app_dir_head(phys):
    for off, e in entries(phys):
        if e[16:28] == b"app_dir_head":
            return off, struct.unpack_from("<I", e, 4)[0]
    die("no app_dir_head in sector 0: not the layout this tool knows")


def build():
    template, cfg = J.template_and_cfg(DUMP)
    vm_e = next((e for e in template if e.name.rstrip(b"\x00\xff") == b"VM"), None)
    if vm_e is None:
        die("no VM entry in the dump's app directory")
    vm_flash = DEST + vm_e.offset
    installer = bootpkg.installer_area(template, cfg, vm=(vm_e.offset, vm_e.size))
    stage = bootpkg.stage(vm_flash)
    say(f"== installer {len(installer):#x} B at {INSTALL_DIR:#x}, staging {len(stage):#x} B at "
        f"{STAGE:#x}, staging erase_end {vm_flash:#x}")
    return installer, stage


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--device", help="/dev/sgN (default: autodetect)")
    ap.add_argument("--dry-run", action="store_true", help="build and check, write nothing")
    a = ap.parse_args()
    if not TOOL.exists():
        die(f"missing {TOOL}")
    OUT.mkdir(parents=True, exist_ok=True)

    installer, stage = build()

    K = sector0_keystream()
    raw = rom_read(a, 0, SECTOR, OUT / "sector0.bin")
    phys = None
    for label, cand in (("as read", raw),
                        ("whole-sector descramble", bytes(x ^ y for x, y in zip(raw, K)))):
        try:
            off, boot_dir = app_dir_head(cand)
        except SystemExit:
            continue
        say(f"== sector 0 ({label}): app_dir_head at +{off:#x} -> {boot_dir:#x}")
        phys = cand
        break
    if phys is None:
        die("no app_dir_head in sector 0: not the layout this tool knows")
    if boot_dir != INSTALL_DIR:
        die(f"sector 0 boots {boot_dir:#x}, not the installer at {INSTALL_DIR:#x}: "
            "not the state this recovery expects (see host/rom_inspect.py)")
    diag = rom_read(a, DIAG_PAGE, 0x200, OUT / "diag_page.bin")
    if int.from_bytes(diag[0:4], "little") == DIAG_MAGIC:
        sr = int.from_bytes(diag[4:8], "little")
        say(f"   last installer run: step {sr >> 4} result {sr & 0xF:#x}")

    end = STAGE + len(stage)
    say(f"== target {INSTALL_DIR:#x}..{end:#x} ({(end - INSTALL_DIR + SECTOR - 1) // SECTOR} sectors)")
    used = rom_read(a, INSTALL_DIR, end - INSTALL_DIR, OUT / "install_old.bin")
    if used.count(0xFF) != len(used):
        say("   note: not blank (the failed attempt); it gets erased")
    if a.dry_run:
        say("   dry run: stopping here, nothing was written")
        return

    (OUT / "installer_new.bin").write_bytes(installer)
    (OUT / "stage_new.bin").write_bytes(stage)
    say("== erasing and writing (MaskROM download mode)")
    rom(a, [f"write {INSTALL_DIR:#x} {OUT / 'installer_new.bin'}",
            f"write {STAGE:#x} {OUT / 'stage_new.bin'}", "exit"])
    if rom_read(a, INSTALL_DIR, len(installer), OUT / "install_new.bin") != installer or \
       rom_read(a, STAGE, len(stage), OUT / "stage_new_read.bin") != stage:
        die("read-back mismatch: do NOT power-cycle; keep this output and ask")
    say("   read back OK")
    say("\n>> now switch the pad OFF and ON again: the installer replaces the stock firmware\n"
        "   (a few seconds), then the bootloader comes up as DFU 1209:0002\n"
        "   next: host/dfu_flash.sh work/flash/fw_area.bin")


if __name__ == "__main__":
    main()
