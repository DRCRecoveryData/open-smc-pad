#!/usr/bin/env python3
"""The recovery net: bring the pad back through the ROM download mode, from any flash state.

    host/maskrom.py status                  # what is on the flash now (read-only)
    host/maskrom.py backup                  # the whole 512 KB, raw, into work/recover/
    host/maskrom.py restore-stock [--image F]   # the factory firmware, byte for byte
    host/maskrom.py install-boot [--dfu]    # bootloader at 0x1000 (+ sector 0 if it drifted)
    host/maskrom.py selftest                # check the conversions against the dumps, no pad

Needs the pad in ROM download mode (`BR25 UBOOT1.00`, `4c4a:2542`, docs/flash-layout.md: with
the pad off, arm the Pico running hw/jl-usbkey, then power the pad on).  The SCSI device needs
root; the script asks `sudo -n` by itself when /dev/sg* is not writable.

Every write is preceded by a full backup in work/recover/ and followed by a read-back that
must match, sector for sector.  Nothing here reads SINCO or needs a firmware on the pad.

The trap this tool exists for: the flash's *storage* form is not what the CPU reads.  The ROM
loader is raw.  Sector 0 is stored as the SINCO view XOR an ENC keystream, the app area
0x1000..0x26000 is stored SFC-encrypted (`jlimage.sfc_encrypt_at`), above 0x26000 raw; a
sector 0 written as the view is what kept the pad dark after a "clean" reflash.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import jlimage as J                                  # noqa: E402

SECTOR = J.SECTOR0_SIZE         # 0x1000
FLASH = 0x80000                 # the AC6966C's SPI NOR
WINDOW_END = J.ENC_END          # 0x26000: end of the SFC-encrypted window
STOCK_END = 0x75000             # factory VM/PRCT end; above it the pad's data (BTIF, EXIF,
                                # USRFLASH, USRTRIM, key_mac) -- never touched by this tool
APP_ADDR = 0x9000               # the bootloader's application directory (fw/boot/boot.h APP_DIR)
BOOT_AREA = ROOT / "work/flash/boot_area.bin"
TOOL = ROOT / "tools/jl-uboot-tool/jluboottool.py"
RECOVER = ROOT / "work/recover"
FACTORY = ROOT / "dumps/original_physical_full.bin"
CHIP_KEY = 0x980F
DFU_ID = "1209:0002"
CHUNK = 0x4000                  # per `write` command; kept small (all in one loader session)


def say(msg):
    print(msg, flush=True)


def die(msg):
    sys.exit(f"!! {msg}")


# ------------------------------------------------------------------ storage domains

def xor_sector0(buf):
    return bytes(a ^ b for a, b in zip(buf, J.sector0_keystream()))


def view_to_stored(view):
    """The 512 KB as a SINCO read returns it -> the bytes physically in the cells."""
    out = bytearray(view)
    out[:SECTOR] = xor_sector0(view[:SECTOR])
    out[SECTOR:WINDOW_END] = J.sfc_encrypt_at(view[SECTOR:WINDOW_END], SECTOR)
    return bytes(out)


def stored_to_view(stored):
    """Symmetric: the bytes in the cells -> what the CPU (and SINCO) reads."""
    out = bytearray(stored)
    out[:SECTOR] = xor_sector0(stored[:SECTOR])
    out[SECTOR:WINDOW_END] = J.sfc_encrypt_at(stored[SECTOR:WINDOW_END], SECTOR)
    return bytes(out)


def app_dir_head(stored):
    try:
        return J.sector0_app_dir_head(stored)
    except ValueError:
        die("sector 0 has no valid file list (app_dir_head): restore-stock repairs it")


def load_factory(path):
    """A factory image in either domain, returned in the stored domain, checked.

    Takes the SINCO view dump as well as a raw physical dump: detects which one it got.
    """
    p = Path(path)
    if not p.exists():
        die(f"no factory image at {p} (a SINCO dump of the stock firmware, or a raw 512 KB "
            f"dump of it: --image)")
    img = p.read_bytes()
    if len(img) != FLASH:
        die(f"{p} is {len(img)} bytes, expected {FLASH:#x} (a full 512 KB flash dump)")
    try:                                  # a raw dump: the stored file list decodes
        J.sector0_app_dir_head(img[:SECTOR])
        stored, kind = img, "raw (ROM loader domain)"
    except ValueError:
        stored, kind = view_to_stored(img), "SINCO view"
    view = stored_to_view(stored)
    say(f"   factory image {p.name}: {kind}")

    entries = J.read_dir(view, SECTOR)
    for i, e in enumerate(entries):
        if J.crc16(e.pack(False)[2:]) != e.hdr_crc:
            die(f"factory image: app directory entry {i} fails its CRC -- wrong image or domain")
    vm = next((e for e in entries if e.name.rstrip(b"\x00\xff") == b"VM"), None)
    if vm is None or SECTOR + vm.offset != WINDOW_END:
        die(f"factory image: VM entry is not at {WINDOW_END:#x} -- the stock layout this tool "
            f"converts assumes the SFC window there")
    head = entries[0]
    if J.crc16(view[SECTOR + 0x20: SECTOR + head.size]) != head.data_crc:
        die("factory image: app_area_head data CRC fails -- corrupt dump")
    return stored


def check_boot_area(boot):
    head = J.read_dir(boot)[0]
    if head.offset != J.AREA_VMA + 0x120 or J.crc16(boot[0x20:head.size]) != head.data_crc:
        die(f"{BOOT_AREA} is not a bootloader area for 0x1000 (host/package_fw.sh boot)")


def save_backup(img):
    RECOVER.mkdir(parents=True, exist_ok=True)
    p = RECOVER / time.strftime("backup_%Y%m%d-%H%M%S.bin")
    p.write_bytes(img)
    digest = hashlib.sha256(img).hexdigest()
    p.with_suffix(".bin.sha256").write_text(f"{digest}  {p.name}\n")
    say(f"   backup: {p}  sha256 {digest}")
    return p


# ------------------------------------------------------------------ the loader

def find_device():
    cands = []
    for d in sorted(Path("/sys/class/scsi_generic").glob("sg*")):
        try:
            text = ((d / "device/vendor").read_text() + " " +
                    (d / "device/model").read_text()).lower()
        except OSError:
            continue
        usb = ""
        for parent in d.resolve().parents:
            if (parent / "idVendor").exists():
                usb = ((parent / "idVendor").read_text().strip() + ":" +
                       (parent / "idProduct").read_text().strip())
                break
        if "br25" in text or usb == "4c4a:2542":
            cands.append(("/dev/" + d.name, usb))
    if not cands:
        die("no BR25 UBOOT device: the pad is not in ROM download mode (power it off, arm the "
            "Pico with `r`, power it on; docs/flash-layout.md)")
    for dev, usb in cands:                       # the loader, not some other SCSI disk
        if usb == "4c4a:2542":
            return dev
    return cands[0][0]


def user_site():
    return subprocess.run([sys.executable, "-c",
                           "import site; print(site.getusersitepackages())"],
                          capture_output=True, text=True).stdout.strip()


class Loader:
    def __init__(self, dev):
        self.dev = dev
        self.tmp = Path(tempfile.mkdtemp(prefix="smcpad-maskrom-"))

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def session(self, *cmds):
        tool = [sys.executable, str(TOOL), "--device", self.dev, *cmds]
        if not os.access(self.dev, os.R_OK | os.W_OK):
            site = user_site()
            tool = ["sudo", "-n", "env"] + ([f"PYTHONPATH={site}"] if site else []) + tool
        r = subprocess.run(tool, capture_output=True, text=True, timeout=900)
        out = r.stdout + r.stderr
        if r.returncode != 0:
            if "a password is required" in out:
                die("the SCSI device needs root and `sudo -n` did not work")
            die(f"jluboottool failed ({r.returncode}):\n" + tail(out))
        if "failed to get the chip key" in out or not out.strip():
            die("the loader did not answer properly:\n" + tail(out))
        if f"{CHIP_KEY:#06x}" not in out.lower():
            die(f"unexpected chip key (not {CHIP_KEY:#06x}): not this pad?:\n" + tail(out))
        return out

    def read(self, addr, length, name="read"):
        p = self.tmp / f"{name}.bin"
        self.session(f"read {addr:#x} {length:#x} {p}")
        data = p.read_bytes() if p.exists() else b""
        if len(data) != length:
            die(f"read {length:#x} bytes at {addr:#x} returned {len(data):#x}")
        return data

    def write_and_verify(self, plan):
        """plan [(flash_addr, bytes)] -> write, read back, compare. Raises via die()."""
        cmds = []
        for i, (addr, data) in enumerate(plan):
            for o in range(0, len(data), CHUNK):
                p = self.tmp / f"w{i}-{o:x}.bin"
                p.write_bytes(data[o:o + CHUNK])
                cmds.append(f"write {addr + o:#x} {p}")
        for i, (addr, data) in enumerate(plan):
            cmds.append(f"read {addr:#x} {len(data):#x} {self.tmp / f'r{i}.bin'}")
        self.session(*cmds, "exit")
        for i, (addr, data) in enumerate(plan):
            got = (self.tmp / f"r{i}.bin").read_bytes()
            if len(got) != len(data):
                die(f"read back {len(got):#x} bytes at {addr:#x}, wrote {len(data):#x}")
            if got != data:
                bad = [addr + o for o in range(0, len(data), SECTOR)
                       if got[o:o + SECTOR] != data[o:o + SECTOR]]
                die(f"read-back mismatch at {', '.join(hex(b) for b in bad[:8])}"
                    f"{' ...' if len(bad) > 8 else ''} (flash was written; a backup is in "
                    f"{RECOVER})")
        say(f"   read back OK ({sum(len(d) for _, d in plan):#x} bytes)")


def tail(text, lines=20):
    return "\n".join(text.strip().splitlines()[-lines:])


def app_at(view, addr):
    """The app directory at addr, or None if it does not check out."""
    entries = J.read_dir(view, addr)
    for e in entries:
        if J.crc16(e.pack(False)[2:]) != e.hdr_crc:
            return None
    head = entries[0]
    if J.crc16(view[addr + 0x20: addr + head.size]) != head.data_crc:
        return None
    return head


# ------------------------------------------------------------------ commands

def cmd_status(a):
    L = Loader(a.device or find_device())
    try:
        img = L.read(0, FLASH, "full")
    finally:
        L.close()
    view = stored_to_view(img)
    try:
        off, target = J.sector0_app_dir_head(img[:SECTOR])
    except ValueError:
        say("== sector 0: no valid file list -- the ROM stays in download mode; "
            "restore-stock brings it back")
        target = None
    if target is not None:
        say(f"== sector 0: {len(J.sector0_entries(img[:SECTOR]))} files, "
            f"app_dir_head at +{off:#x} -> {target:#x}")

    if target is None:
        pass
    elif target == SECTOR:
        boot = BOOT_AREA.read_bytes() if BOOT_AREA.exists() else b""
        if boot and view[SECTOR:SECTOR + len(boot)] == boot:
            say(f"   layout: our bootloader at 0x1000 (matches {BOOT_AREA.name})")
        else:
            say("   layout: an app area at 0x1000")
        app = app_at(view, J.AREA_BASE)
        if app:
            say(f"   application at {J.AREA_BASE:#x}: {app.size:#x} bytes, CRCs OK")
        elif boot and view[SECTOR:SECTOR + len(boot)] == boot:
            say("   no application at 0x9000: the bootloader comes up as DFU " + DFU_ID)
    else:
        say(f"   layout: sector 0 points at {target:#x} (not the app area at 0x1000)")

    factory = load_factory(a.image) if Path(a.image).exists() else None
    if factory:
        fview = stored_to_view(factory)
        diffs = [i for i in range(0, STOCK_END, SECTOR) if view[i:i + SECTOR] != fview[i:i + SECTOR]]
        if diffs:
            say(f"   vs the factory image: {len(diffs)} of {STOCK_END // SECTOR} sectors differ"
                f" (first {', '.join(hex(d) for d in diffs[:6])}{' ...' if len(diffs) > 6 else ''})")
        else:
            say("   matches the factory image, 0..0x75000")


def cmd_backup(a):
    L = Loader(a.device or find_device())
    try:
        img = L.read(0, FLASH, "full")
    finally:
        L.close()
    save_backup(img)


def plan_restore(stored, cur):
    """What restore-stock writes, given the current raw flash."""
    plan = [(0, stored[:SECTOR]), (SECTOR, stored[SECTOR:STOCK_END])]
    return [(addr, data) for addr, data in plan if cur[addr:addr + len(data)] != data]


def plan_install_boot(cur, boot):
    """What install-boot writes: sector 0 only if it does not point at 0x1000, then the area."""
    stored_boot = J.sfc_encrypt_at(boot, SECTOR)
    _, target = app_dir_head(cur[:SECTOR])
    plan = []
    if target != SECTOR:
        plan.append((0, J.sector0_with_app_dir(cur[:SECTOR], SECTOR)))
    if cur[SECTOR:SECTOR + len(stored_boot)] != stored_boot:
        plan.append((SECTOR, stored_boot))
    return plan, target


def cmd_restore_stock(a):
    stored = load_factory(a.image)
    L = Loader(a.device or find_device())
    try:
        cur = L.read(0, FLASH, "before")
        plan = plan_restore(stored, cur)
        n = sum(len(d) for _, d in plan)
        say(f"== restore stock: {n:#x} bytes differ from the factory image "
            f"(0..{STOCK_END:#x}); the data area above it is left alone")
        if not plan:
            say("   nothing to do")
            return
        if a.dry_run:
            say("   dry run: not writing")
            return
        save_backup(cur)
        if not a.yes and input(f"write {n:#x} bytes to the pad? type 'yes': ").strip() != "yes":
            die("nothing written")
        L.write_and_verify(plan)
    finally:
        L.close()
    say("\n>> power-cycle the pad: the stock firmware comes up as USB 4353:4b4d "
        "(\"SINCO\", MIDI ports)")


def cmd_install_boot(a):
    boot = BOOT_AREA.read_bytes()
    check_boot_area(boot)
    L = Loader(a.device or find_device())
    try:
        cur = L.read(0, FLASH, "before")
        plan, target = plan_install_boot(cur, boot)
        say(f"== install boot: {boot_area_summary(boot)}"
            f"{'' if target == SECTOR else f', sector 0 repointed from {target:#x}'}")
        if not plan:
            say("   nothing to do (flash already matches)")
        elif a.dry_run:
            say("   dry run: not writing")
        else:
            save_backup(cur)
            if not a.yes and input("write? type 'yes': ").strip() != "yes":
                die("nothing written")
            L.write_and_verify(plan)
        after = bytearray(cur)
        for addr, data in plan:
            after[addr:addr + len(data)] = data
        app = app_at(stored_to_view(bytes(after)), APP_ADDR)
    finally:
        L.close()
    if app:
        say(f"\n>> power-cycle the pad: the application at {APP_ADDR:#x} "
            f"({app.size:#x} bytes) should come up as 1209:0001")
        return
    say("\n>> power-cycle the pad: the bootloader comes up as DFU " + DFU_ID +
        "\n   then host/dfu_flash.sh (or re-run with --dfu and power-cycle when asked)")
    if a.dfu and not a.dry_run:
        wait_dfu_then_flash()


def boot_area_summary(boot):
    head = J.read_dir(boot)[0]
    vm = J.read_dir(boot)[3]
    return (f"{len(boot):#x} bytes at 0x1000, entry {head.offset:#x}, "
            f"VM {vm.offset:#x}/{vm.size:#x}")


def wait_dfu_then_flash():
    if not shutil.which("dfu-util"):
        die("dfu-util not found")
    say("   waiting for DFU " + DFU_ID + " (power-cycle the pad now, 120 s) ...")
    for _ in range(240):
        r = subprocess.run(["dfu-util", "-l"], capture_output=True, text=True)
        if f"[{DFU_ID}]" in r.stdout:
            break
        time.sleep(0.5)
    else:
        die("DFU did not appear")
    sys.exit(subprocess.run([str(HERE / "dfu_flash.sh")]).returncode)


def cmd_selftest(a):
    view = Path(a.view).read_bytes()
    phys = Path(a.physical).read_bytes()
    checks = []
    stored = view_to_stored(view)
    checks.append(("view -> stored == the raw dump", stored == phys))
    checks.append(("stored -> view round trip", stored_to_view(stored) == view))

    s0 = phys[:SECTOR]
    moved = J.sector0_with_app_dir(s0, 0x41000)
    checks.append(("sector 0: app_dir_head repoint -> back",
                   J.sector0_app_dir_head(moved)[1] == 0x41000
                   and J.sector0_with_app_dir(moved, SECTOR) == s0))
    boot = BOOT_AREA.read_bytes()
    checks.append(("boot area SFC round trip",
                   J.sfc_encrypt_at(J.sfc_encrypt_at(boot, SECTOR), SECTOR) == boot))
    bad = 0
    for name, ok in checks:
        say(f"   {'ok  ' if ok else 'FAIL'} {name}")
        bad += not ok
    sys.exit(1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["status", "backup", "restore-stock", "install-boot", "selftest"])
    ap.add_argument("--device", help="/dev/sg* of the ROM loader (default: find the BR25 one)")
    ap.add_argument("--image", default=str(FACTORY),
                    help="factory image for restore-stock/status (SINCO view or raw)")
    ap.add_argument("--view", default=str(Path.home() / "smcpad/dumps/pocket_fw004_flash.bin"),
                    help="selftest: the SINCO view dump")
    ap.add_argument("--physical", default=str(FACTORY), help="selftest: the raw dump")
    ap.add_argument("--dfu", action="store_true", help="install-boot: wait for DFU, then flash")
    ap.add_argument("--dry-run", action="store_true", help="read and check, write nothing")
    ap.add_argument("--yes", action="store_true", help="do not ask before writing")
    a = ap.parse_args()
    {"status": cmd_status, "backup": cmd_backup, "restore-stock": cmd_restore_stock,
     "install-boot": cmd_install_boot, "selftest": cmd_selftest}[a.cmd](a)


if __name__ == "__main__":
    main()
