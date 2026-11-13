#!/usr/bin/env python3
"""Device detection and app-area validation for the SMC-PAD Pocket web UI.

Linux only (sysfs, termios).  No HTTP and no device writes live here; server.py
imports this.
"""
import fcntl
import glob
import hashlib
import os
import re
import struct
import subprocess
import tempfile
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BACKUPS = os.path.join(ROOT, "work", "webui", "backups")
UPLOADS = os.path.join(ROOT, "work", "webui", "uploads")
TMP = os.path.join(ROOT, "work", "webui", "tmp")
BOOT_PACKAGE = os.path.join(ROOT, "work", "flash", "boot_update.bin")
KNOWN_APP = os.path.join(ROOT, "work", "sketch", "MidiFighterSpectra",
                         "MidiFighterSpectra.bin")

DFU_ID = "1209:0002"
APP_ID = "1209:0001"
STOCK_ID = "4353:4b4d"

APP_ENTRY = 0x1E08120           # fw/boot/boot.h
APP_AREA_MAX = 0x71000 - 0x9000
FLASH_SIZE = 0x80000
DIR_DATA_CRC, DIR_OFFSET, DIR_SIZE = 2, 4, 8

NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*\.bin$")


def run(cmd, timeout=5):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None


def wait_until(pred, timeout, step=0.25):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


# ---------------------------------------------------------------- USB state

def dfu_present():
    r = run(["dfu-util", "-l"])
    return bool(r and f"[{DFU_ID}]" in r.stdout)


def usb_devices():
    out = []
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            vid = open(os.path.join(d, "idVendor")).read().strip()
            pid = open(os.path.join(d, "idProduct")).read().strip()
        except OSError:
            continue
        dev = {"id": f"{vid}:{pid}", "product": "", "serial": ""}
        for key, fname in (("product", "product"), ("serial", "serial")):
            try:
                dev[key] = open(os.path.join(d, fname)).read().strip()
            except OSError:
                pass
        if vid in ("1209", "4353"):
            out.append(dev)
    return out


def classify():
    devs = [d for d in usb_devices() if d["id"] in (APP_ID, STOCK_ID)]
    if dfu_present():
        dfu = next((d for d in usb_devices() if d["id"] == DFU_ID), None)
        return {"mode": "dfu", "id": DFU_ID,
                "product": (dfu or {}).get("product", "SMC-PAD bootloader (DFU)"),
                "serial": (dfu or {}).get("serial", "")}
    if not devs:
        return {"mode": "absent", "id": "", "product": "", "serial": ""}
    if len(devs) > 1:
        return {"mode": "mixed", "id": ",".join(d["id"] for d in devs),
                "product": "", "serial": ""}
    d = devs[0]
    return {"mode": "app" if d["id"] == APP_ID else "stock",
            "id": d["id"], "product": d["product"], "serial": d["serial"]}


def app_port():
    for p in glob.glob("/dev/serial/by-id/*SMC-PAD*"):
        return os.path.realpath(p)
    for t in glob.glob("/sys/class/tty/ttyACM*"):
        try:
            prod = open(os.path.join(t, "device", "..", "product")).read()
        except OSError:
            continue
        if "SMC-PAD" in prod:
            return "/dev/" + os.path.basename(t)
    return None


def midi_port():
    try:
        cards = open("/proc/asound/cards").read()
    except OSError:
        return None
    m = re.search(r"^\s*(\d+) \[[^\]]*\]: USB-Audio - .*SMC-PAD", cards, re.M)
    if not m:
        return None
    n = m.group(1)
    return f"hw:{n},0,0" if os.path.exists(f"/dev/snd/midiC{n}D0") else None


def boot_version(timeout=6):
    if not dfu_present():
        return None
    fd, path = tempfile.mkstemp(prefix="smcpad-ver-")
    os.close(fd)
    os.unlink(path)
    try:
        run(["dfu-util", "-d", DFU_ID, "-a", "2", "-U", path], timeout=timeout)
        if os.path.getsize(path) > 0:
            return open(path, "rb").read().decode(errors="replace").strip()
    except OSError:
        pass
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    return None


# ---------------------------------------------------------------- app area

def crc16(data, crc=0):
    """JieLi CRC16: CCITT polynomial 0x1021, init 0, not reflected."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def validate_app_area(data, name=""):
    """Check an app-area image the way the bootloader does, plus the area CRC."""
    if name.lower().endswith(".fwsc"):
        return False, "a .fwsc is the vendor full-firmware container, not an app area"
    if len(data) >= FLASH_SIZE:
        return False, "this is a full raw flash dump, not an app area"
    if len(data) < 0x120:
        return False, "too short to hold an app-area directory"
    head = data[:0x20]
    if crc16(head[2:32]) != struct.unpack_from("<H", head, 0)[0]:
        return False, "directory header CRC mismatch"
    if struct.unpack_from("<I", head, DIR_OFFSET)[0] != APP_ENTRY:
        return False, "directory entry is not the application entry 0x1E08120"
    size = struct.unpack_from("<I", head, DIR_SIZE)[0]
    if not 0x120 < size <= APP_AREA_MAX:
        return False, f"declared app-area size {size} is out of range"
    if size > len(data):
        return False, "the directory declares more data than the file holds"
    if crc16(data[0x20:size]) != struct.unpack_from("<H", head, DIR_DATA_CRC)[0]:
        return False, "app-area CRC mismatch (truncated or corrupted file)"
    return True, ""


# ---------------------------------------------------------------- files

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def safe_path(kind, name):
    """Resolve a user-supplied file name inside backups/ or uploads/, or None."""
    root = {"backups": BACKUPS, "uploads": UPLOADS}.get(kind)
    if root is None or not NAME_RE.match(name or ""):
        return None
    path = os.path.realpath(os.path.join(root, name))
    if not path.startswith(os.path.realpath(root) + os.sep):
        return None
    return path if os.path.isfile(path) else None


def list_backups():
    out = []
    try:
        entries = sorted(os.scandir(BACKUPS), key=lambda e: e.name, reverse=True)
    except OSError:
        return out
    for e in entries:
        try:
            if not e.is_file(follow_symlinks=False) or not NAME_RE.match(e.name):
                continue
            st = e.stat(follow_symlinks=False)
        except OSError:
            continue
        out.append({"name": e.name,
                    "kind": "flash" if e.name.startswith("flash_") else "app",
                    "bytes": st.st_size,
                    "mtime": int(st.st_mtime),
                    "sha256": sha256_file(e.path)})
    return out


# ---------------------------------------------------------------- serial helpers

def touch_1200(port):
    """Arduino-style 1200-baud touch: open, set 1200, raise then drop DTR."""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    try:
        a = termios.tcgetattr(fd)
        a[2] &= ~(termios.CSIZE | termios.PARENB)
        a[2] |= termios.CS8
        a[4] = a[5] = termios.B1200
        termios.tcsetattr(fd, termios.TCSANOW, a)
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))
        time.sleep(0.1)
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR))
        time.sleep(0.1)
    finally:
        os.close(fd)


def console_B(port):
    """Fallback for console firmwares: send B a few times (a running monitor eats one)."""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        a = termios.tcgetattr(fd)
        a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
                  | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
        a[1] &= ~termios.OPOST
        a[2] &= ~(termios.CSIZE | termios.PARENB)
        a[2] |= termios.CS8
        a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG
                  | termios.IEXTEN)
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))
        time.sleep(1.0)
        for _ in range(6):
            os.write(fd, b"B\r")
            time.sleep(0.5)
    finally:
        os.close(fd)
