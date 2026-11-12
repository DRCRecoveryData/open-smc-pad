#!/usr/bin/env python3
"""Read the Pocket's state through MaskROM download mode (Pico USB_KEY), read-only.

    python3 host/rom_inspect.py [--device /dev/sgN]

Needs tools/jl-uboot-tool (tools/jl-uboot-tool/jluboottool.py) and the chip in UBOOT
mass-storage mode.  Dumps into work/recover/ and decodes the installer diagnostics
(fw/boot/install.c: DIAG_PAGE 0x4C000, INSTALL_DIAG in RAM cannot be read from here).
"""
import argparse
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
TOOL = ROOT / "tools" / "jl-uboot-tool" / "jluboottool.py"
OUT = ROOT / "work" / "recover"

# (name, flash offset, length)
REGIONS = [
    ("sector0", 0x000000, 0x1000),
    ("dir_1000", 0x001000, 0x0400),
    ("app_head", 0x001120, 0x0040),
    ("install_dir", 0x041000, 0x0400),
    ("diag_page", 0x04C000, 0x0200),
]

STEPS = {
    1: {0: "flash_begin OK", 1: "flash_begin FAILED", 2: "staging check FAILED -> back to 0x1000"},
    2: {0: "erasing/writing bootloader", 1: "erase failed", 2: "staging read failed",
        3: "bootloader write failed"},
    3: {0: "done (or point sector 0 at 0x1000)", 1: "set_boot_dir failed", 0xF: "DONE"},
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device")
    args = ap.parse_args()
    if not TOOL.exists():
        sys.exit(f"missing {TOOL}")
    OUT.mkdir(parents=True, exist_ok=True)

    cmds = []
    for name, off, ln in REGIONS:
        cmds.append(f"read {off:#x} {ln:#x} {OUT / (name + '.bin')}")
    cmds.append("exit")
    cmd = [sys.executable, str(TOOL)]
    if args.device:
        cmd += ["--device", args.device]
    r = subprocess.run(cmd + cmds, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode:
        sys.exit(r.returncode)

    diag = (OUT / "diag_page.bin").read_bytes()
    magic = int.from_bytes(diag[0:4], "little")
    step_res = int.from_bytes(diag[4:8], "little")
    print()
    if magic != 0x31474449:                     # "IDG1"
        print(f"diag_page: no magic (0x{magic:08X}) -- installer never reached diag_save()")
    else:
        step, res = step_res >> 4, step_res & 0xF
        print(f"installer: step {step} result {res:#x} ({STEPS.get(step, {}).get(res, '?')})")
    print(f"sr1_before=0x{int.from_bytes(diag[8:12],'little'):02X} "
          f"sr2_before=0x{int.from_bytes(diag[12:16],'little'):02X} "
          f"sr1_after=0x{int.from_bytes(diag[16:20],'little'):02X} "
          f"sr2_after=0x{int.from_bytes(diag[20:24],'little'):02X} "
          f"unprotect={int.from_bytes(diag[24:28],'little')}")


if __name__ == "__main__":
    main()
